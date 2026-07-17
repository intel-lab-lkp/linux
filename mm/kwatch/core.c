// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>
#include "kwatch.h"

static struct kwatch_config kwatch_config;
static bool watching_active;

static struct dentry *dbgfs_dir;
static struct dentry *dbgfs_config;
static DEFINE_MUTEX(kwatch_dbgfs_mutex);
static atomic_t dbgfs_config_busy = ATOMIC_INIT(0);

static int kwatch_start_watching(void)
{
	int ret;

	if (!strlen(kwatch_config.func_name)) {
		if (kwatch_config.duration > 0) {
			strscpy(kwatch_config.func_name, "kwatch_global_anchor",
				sizeof(kwatch_config.func_name));
		} else {
			pr_err("func_name or duration is required\n");
			return -EINVAL;
		}
	} else if (kwatch_config.duration > 0 &&
		   strcmp(kwatch_config.func_name, "kwatch_global_anchor")) {
		pr_warn("duration is ignored when watching a specific function\n");
	}

	ret = kwatch_hwbp_prealloc(kwatch_config.max_watch);
	if (ret) {
		pr_err("kwatch_hwbp_prealloc ret: %d\n", ret);
		return ret;
	}

	ret = kwatch_tsk_ctx_prealloc(kwatch_config.max_concurrency);
	if (ret) {
		kwatch_hwbp_free();
		return ret;
	}

	ret = kwatch_probe_start(&kwatch_config);
	if (ret) {
		pr_err("kwatch_probe_start ret: %d\n", ret);
		kwatch_tsk_ctx_free();
		kwatch_hwbp_free();
		return ret;
	}

	if (!strcmp(kwatch_config.func_name, "kwatch_global_anchor")) {
		ret = kwatch_anchor_start(kwatch_config.duration);
		if (ret) {
			kwatch_probe_stop();
			synchronize_rcu();
			kwatch_tsk_ctx_release_wps();
			kwatch_hwbp_free();
			kwatch_tsk_ctx_free();
			return ret;
		}
	}

	watching_active = true;
	return 0;
}

static void kwatch_stop_watching(void)
{
	watching_active = false;

	kwatch_anchor_stop();
	/* after kthread_stop: the dead thread cannot re-mark expiry */
	kwatch_anchor_clear_expired();

	kwatch_probe_stop();
	synchronize_rcu();
	kwatch_tsk_ctx_release_wps();
	/*
	 * Waits for disarm IPIs and unregisters breakpoints: no #DB can
	 * reach the ctx pool once this returns.
	 */
	kwatch_hwbp_free();
	kwatch_tsk_ctx_free();
}

void kwatch_auto_stop(void)
{
	mutex_lock(&kwatch_dbgfs_mutex);
	/* the expired check neutralizes work items from torn-down sessions */
	if (watching_active && kwatch_anchor_has_expired()) {
		kwatch_stop_watching();
		pr_info("watch duration expired, stopped watching\n");
	}
	mutex_unlock(&kwatch_dbgfs_mutex);
}

static int kwatch_config_parse(char *buf, struct kwatch_config *cfg)
{
	char *token, *key, *val;
	int ret = 0;

	memset(cfg, 0, sizeof(*cfg));
	cfg->max_concurrency = 256;
	cfg->max_watch = 4;
	cfg->watch_len = 8;

	while ((token = strsep(&buf, " \t\n")) != NULL) {
		if (!*token)
			continue;
		key = strsep(&token, "=");
		val = token;
		if (!key || !val)
			return -EINVAL;

		if (!strcmp(key, "func_name")) {
			strscpy(cfg->func_name, val, sizeof(cfg->func_name));
		} else if (!strcmp(key, "func_offset")) {
			ret = kstrtou16(val, 0, &cfg->func_offset);
		} else if (!strcmp(key, "depth")) {
			ret = kstrtou16(val, 0, &cfg->depth);
		} else if (!strcmp(key, "max_concurrency")) {
			ret = kstrtou16(val, 0, &cfg->max_concurrency);
		} else if (!strcmp(key, "max_watch")) {
			ret = kstrtou16(val, 0, &cfg->max_watch);
		} else if (!strcmp(key, "watch_len")) {
			ret = kstrtou16(val, 0, &cfg->watch_len);
			if (!ret && cfg->watch_len != 1 &&
			    cfg->watch_len != 2 && cfg->watch_len != 4 &&
			    cfg->watch_len != 8)
				ret = -EINVAL;
		} else if (!strcmp(key, "duration")) {
			ret = kstrtou16(val, 0, &cfg->duration);
		} else if (!strcmp(key, "watch_expr")) {
			strscpy(cfg->watch_expr, val, sizeof(cfg->watch_expr));
			ret = kwatch_deref_parse(cfg, val);
		}

		if (ret)
			return ret;
	}
	return 0;
}

static int kwatch_dbgfs_open(struct inode *inode, struct file *file)
{
	if (atomic_cmpxchg(&dbgfs_config_busy, 0, 1))
		return -EBUSY;
	return 0;
}

static int kwatch_dbgfs_release(struct inode *inode, struct file *file)
{
	atomic_set(&dbgfs_config_busy, 0);
	return 0;
}

static ssize_t kwatch_dbgfs_read(struct file *file, char __user *user_buf,
				 size_t count, loff_t *ppos)
{
	char *out_buf;
	size_t len = 0;
	ssize_t ret;

	out_buf = kzalloc(MAX_CONFIG_STR_LEN, GFP_KERNEL);
	if (!out_buf)
		return -ENOMEM;

	/*
	 * Serialize against the write path and the auto-stop work item so the
	 * config snapshot cannot tear or race a session teardown.
	 */
	mutex_lock(&kwatch_dbgfs_mutex);

	if (watching_active) {
		len += scnprintf(out_buf + len, MAX_CONFIG_STR_LEN - len,
				 "func_name=%s\n"
				 "func_offset=%u\n"
				 "depth=%u\n"
				 "duration=%u\n"
				 "max_concurrency=%u\n"
				 "max_watch=%u\n"
				 "watch_len=%u\n",
				 kwatch_config.func_name,
				 kwatch_config.func_offset, kwatch_config.depth,
				 kwatch_config.duration,
				 kwatch_config.max_concurrency,
				 kwatch_config.max_watch,
				 kwatch_config.watch_len);

		if (kwatch_config.base == KWATCH_BASE_GLOBAL_SYM) {
			len += scnprintf(out_buf + len, MAX_CONFIG_STR_LEN - len,
					 "sym_addr=0x%lx\n", kwatch_config.sym_addr);
		}

		len += scnprintf(out_buf + len, MAX_CONFIG_STR_LEN - len,
				 "watch_expr=%s\n"
				 "nmi_rejected=%lu\n"
				 "arm_ipi_suppressed=%lu\n",
				 kwatch_config.watch_expr,
				 kwatch_probe_nmi_rejected(),
				 kwatch_hwbp_arm_ipi_suppressed());
	} else {
		len = scnprintf(out_buf, MAX_CONFIG_STR_LEN, "not watching\n");
	}

	mutex_unlock(&kwatch_dbgfs_mutex);

	ret = simple_read_from_buffer(user_buf, count, ppos, out_buf, len);
	kfree(out_buf);
	return ret;
}

static ssize_t kwatch_dbgfs_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *ppos)
{
	char *input_alloc;
	char *parse_str;
	int ret;

	if (count == 0 || count >= MAX_CONFIG_STR_LEN)
		return -EINVAL;

	input_alloc = memdup_user_nul(buffer, count);
	if (IS_ERR(input_alloc))
		return PTR_ERR(input_alloc);

	mutex_lock(&kwatch_dbgfs_mutex);

	if (watching_active)
		kwatch_stop_watching();

	parse_str = strim(input_alloc);

	if (!strlen(parse_str)) {
		ret = -EINVAL;
		goto out;
	}

	ret = kwatch_config_parse(parse_str, &kwatch_config);
	if (ret) {
		pr_err("Failed to parse config %d\n", ret);
		goto out;
	}

	ret = kwatch_start_watching();
	if (ret) {
		pr_err("Failed to start watching with %d\n", ret);
		goto out;
	}

	ret = count;

out:
	mutex_unlock(&kwatch_dbgfs_mutex);
	kfree(input_alloc);
	return ret;
}

static const struct file_operations kwatch_fops = {
	.owner = THIS_MODULE,
	.open = kwatch_dbgfs_open,
	.release = kwatch_dbgfs_release,
	.read = kwatch_dbgfs_read,
	.write = kwatch_dbgfs_write,
};

static int __init kwatch_init(void)
{
	int ret = 0;

	memset(&kwatch_config, 0, sizeof(kwatch_config));

	dbgfs_dir = debugfs_create_dir("kwatch", NULL);
	if (IS_ERR(dbgfs_dir)) {
		ret = PTR_ERR(dbgfs_dir);
		goto err_dir;
	}

	dbgfs_config = debugfs_create_file("config", 0600, dbgfs_dir, NULL,
					   &kwatch_fops);
	if (IS_ERR(dbgfs_config)) {
		ret = PTR_ERR(dbgfs_config);
		goto err_file;
	}

	pr_info("module loaded\n");
	return 0;

err_file:
	debugfs_remove_recursive(dbgfs_dir);
	dbgfs_dir = NULL;
err_dir:
	return ret;
}
module_init(kwatch_init);

static void __exit kwatch_exit(void)
{
	mutex_lock(&kwatch_dbgfs_mutex);
	if (watching_active)
		kwatch_stop_watching();
	mutex_unlock(&kwatch_dbgfs_mutex);

	/* the anchor thread is dead: nothing can schedule new work now */
	kwatch_anchor_cancel_work();

	debugfs_remove_recursive(dbgfs_dir);
	dbgfs_dir = NULL;

	pr_info("kwatch unloaded\n");
}
module_exit(kwatch_exit);

MODULE_AUTHOR("Jinchao Wang <wangjinchao600@gmail.com>");
MODULE_DESCRIPTION("Kernel watchpoint");
MODULE_LICENSE("GPL");
