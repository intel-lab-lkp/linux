// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/kstackwatch.h>
#include <linux/kstrtox.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static atomic_t dbgfs_config_busy = ATOMIC_INIT(0);
static struct ksw_config *ksw_config;
static struct dentry *dbgfs_config;
static struct dentry *dbgfs_dir;

static bool watching_active;

static int ksw_start_watching(void)
{
	int ret;

	/*
	 * Watch init will preallocate the HWBP,
	 * so it must happen before stack init
	 */
	ret = ksw_watch_init();
	if (ret) {
		pr_err("ksw_watch_init ret: %d\n", ret);
		return ret;
	}

	ret = ksw_stack_init();
	if (ret) {
		pr_err("ksw_stack_init ret: %d\n", ret);
		ksw_watch_exit();
		return ret;
	}
	watching_active = true;

	pr_info("start watching: %s\n", ksw_config->user_input);
	return 0;
}

static void ksw_stop_watching(void)
{
	ksw_stack_exit();
	ksw_watch_exit();
	watching_active = false;

	pr_info("stop watching: %s\n", ksw_config->user_input);
}

struct param_map {
	const char *name;       /* long name */
	const char *short_name; /* short name (2 letters) */
	size_t offset;          /* offsetof(struct ksw_config, field) */
	bool is_string;         /* true for string */
};

/* macro generates both long and short name automatically */
#define PMAP(field, short, is_str) \
	{ #field, #short, offsetof(struct ksw_config, field), is_str }

static const struct param_map ksw_params[] = {
	PMAP(func_name,   fn, true),
	PMAP(func_offset, fo, false),
	PMAP(depth,       dp, false),
	PMAP(max_watch,   mw, false),
	PMAP(sp_offset,   so, false),
	PMAP(watch_len,   wl, false),
	PMAP(auto_canary, ac, false),
	PMAP(panic_hit,   ph, false),
};

static int ksw_parse_param(struct ksw_config *config, const char *key,
			   const char *val)
{
	const struct param_map *pm = NULL;
	int ret;

	for (int i = 0; i < ARRAY_SIZE(ksw_params); i++) {
		if (strcmp(key, ksw_params[i].name) == 0 ||
		    strcmp(key, ksw_params[i].short_name) == 0) {
			pm = &ksw_params[i];
			break;
		}
	}

	if (!pm)
		return -EINVAL;

	if (pm->is_string) {
		char **dst = (char **)((char *)config + pm->offset);
		*dst = kstrdup(val, GFP_KERNEL);
		if (!*dst)
			return -ENOMEM;
	} else {
		ret = kstrtou16(val, 0, (u16 *)((char *)config + pm->offset));
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Configuration string format:
 *    param_name=<value> [param_name=<value> ...]
 *
 * Required parameters:
 * - func_name  |fn (str) : target function name
 * - func_offset|fo (u16) : instruction pointer offset
 *
 * Optional parameters:
 * - depth      |dp (u16) : recursion depth
 * - max_watch  |mw (u16) : maximum number of watchpoints
 * - sp_offset  |so (u16) : offset from stack pointer at func_offset
 * - watch_len  |wl (u16) : watch length (1,2,4,8)
 */
static int ksw_parse_config(char *buf, struct ksw_config *config)
{
	char *part, *key, *val;
	int ret;

	kfree(config->func_name);
	kfree(config->user_input);
	memset(ksw_config, 0, sizeof(*ksw_config));

	buf = strim(buf);
	config->user_input = kstrdup(buf, GFP_KERNEL);
	if (!config->user_input)
		return -ENOMEM;

	while ((part = strsep(&buf, " \t\n")) != NULL) {
		if (*part == '\0')
			continue;

		key = strsep(&part, "=");
		val = part;
		if (!key || !val)
			continue;
		ret = ksw_parse_param(config, key, val);
		if (ret)
			pr_warn("unsupported param %s=%s", key, val);
	}

	if (!config->func_name) {
		pr_err("Missing required parameters: function or func_offset\n");
		return -EINVAL;
	}

	return 0;
}

static ssize_t ksw_dbgfs_read(struct file *file, char __user *buf, size_t count,
			      loff_t *ppos)
{
	const char *out;
	size_t len;

	if (watching_active && ksw_config->user_input) {
		out = ksw_config->user_input;
		len = strlen(out);
	} else {
		out = "not watching\n";
		len = strlen(out);
	}

	return simple_read_from_buffer(buf, count, ppos, out, len);
}

static ssize_t ksw_dbgfs_write(struct file *file, const char __user *buffer,
			       size_t count, loff_t *ppos)
{
	char input[MAX_CONFIG_STR_LEN];
	int ret;

	if (count == 0 || count >= sizeof(input))
		return -EINVAL;

	if (copy_from_user(input, buffer, count))
		return -EFAULT;

	if (watching_active)
		ksw_stop_watching();

	input[count] = '\0';
	strim(input);

	if (!strlen(input)) {
		pr_info("config cleared\n");
		return count;
	}

	ret = ksw_parse_config(input, ksw_config);
	if (ret) {
		pr_err("Failed to parse config %d\n", ret);
		return ret;
	}

	ret = ksw_start_watching();
	if (ret) {
		pr_err("Failed to start watching with %d\n", ret);
		return ret;
	}

	return count;
}

static int ksw_dbgfs_open(struct inode *inode, struct file *file)
{
	if (atomic_cmpxchg(&dbgfs_config_busy, 0, 1))
		return -EBUSY;
	return 0;
}

static int ksw_dbgfs_release(struct inode *inode, struct file *file)
{
	atomic_set(&dbgfs_config_busy, 0);
	return 0;
}

static const struct file_operations kstackwatch_fops = {
	.owner = THIS_MODULE,
	.open = ksw_dbgfs_open,
	.read = ksw_dbgfs_read,
	.write = ksw_dbgfs_write,
	.release = ksw_dbgfs_release,
	.llseek = default_llseek,
};

const struct ksw_config *ksw_get_config(void)
{
	return ksw_config;
}

struct dentry *ksw_get_dbgdir(void)
{
	return dbgfs_dir;
}

static int __init kstackwatch_init(void)
{
	int ret = 0;

	ksw_config = kzalloc(sizeof(*ksw_config), GFP_KERNEL);
	if (!ksw_config) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	dbgfs_dir = debugfs_create_dir("kstackwatch", NULL);
	if (!dbgfs_dir) {
		ret = -ENOMEM;
		goto err_dir;
	}

	dbgfs_config = debugfs_create_file("config", 0600, dbgfs_dir, NULL,
				       &kstackwatch_fops);
	if (!dbgfs_config) {
		ret = -ENOMEM;
		goto err_file;
	}

	pr_info("module loaded\n");
	return 0;

err_file:
	debugfs_remove_recursive(dbgfs_dir);
	dbgfs_dir = NULL;
err_dir:
	kfree(ksw_config);
	ksw_config = NULL;
err_alloc:
	return ret;
}

static void __exit kstackwatch_exit(void)
{
	debugfs_remove_recursive(dbgfs_dir);
	kfree(ksw_config->func_name);
	kfree(ksw_config->user_input);
	kfree(ksw_config);

	pr_info("module unloaded\n");
}

module_init(kstackwatch_init);
module_exit(kstackwatch_exit);

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("Kernel Stack Watch");
MODULE_LICENSE("GPL");

