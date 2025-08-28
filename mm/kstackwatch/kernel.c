// SPDX-License-Identifier: GPL-2.0
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "kstackwatch.h"

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("Kernel Stack Watch");
MODULE_LICENSE("GPL");

struct ksw_config *ksw_config;
bool watching_active;

bool panic_on_catch;
module_param(panic_on_catch, bool, 0644);
MODULE_PARM_DESC(panic_on_catch,
		 "Trigger a kernel panic immediately on corruption catch");

static int ksw_start_watching(void)
{
	watching_active = true;

	pr_info("KSW: start watching %s\n", ksw_config->config_str);
	return 0;
}

static void ksw_stop_watching(void)
{
	watching_active = false;

	pr_info("KSW: stop watching %s\n", ksw_config->config_str);
}

/*
 * Format of the configuration string:
 *    function+ip_offset[+depth] [local_var_offset:local_var_len]
 *
 * - function         : name of the target function
 * - ip_offset        : instruction pointer offset within the function
 * - depth            : recursion depth to watch
 * - local_var_offset : offset from the stack pointer at function+ip_offset
 * - local_var_len    : length of the local variable
 */
static int ksw_parse_config(char *buf, struct ksw_config *config)
{
	char *func_part, *local_var_part = NULL;
	char *token;

	/* Set the watch type to the default canary-based monitoring */
	config->type = WATCH_CANARY;

	func_part = strim(buf);
	strscpy(config->config_str, func_part, MAX_CONFIG_STR_LEN);

	local_var_part = strchr(func_part, ' ');
	if (local_var_part) {
		*local_var_part = '\0'; // Terminate the function part
		local_var_part = strim(local_var_part + 1);
	}

	/* 1. Parse the function part: function+ip_offset[+depth] */
	token = strsep(&func_part, "+");
	if (!token)
		return -EINVAL;

	strscpy(config->function, token, MAX_FUNC_NAME_LEN - 1);

	token = strsep(&func_part, "+");
	if (!token || kstrtou16(token, 0, &config->ip_offset)) {
		pr_err("KSW: failed to parse instruction offset\n");
		return -EINVAL;
	}

	token = strsep(&func_part, "+");
	if (token && kstrtou16(token, 0, &config->depth)) {
		pr_err("KSW: failed to parse depth\n");
		return -EINVAL;
	}
	if (!local_var_part || !(*local_var_part))
		return 0;

	/* 2. Parse the optional local var: offset:len */
	config->type = WATCH_LOCAL_VAR;
	token = strsep(&local_var_part, ":");
	if (!token || kstrtou16(token, 0, &config->local_var_offset)) {
		pr_err("KSW: failed to parse stack variable offset\n");
		return -EINVAL;
	}

	if (!local_var_part ||
	    kstrtou16(local_var_part, 0, &config->local_var_len)) {
		pr_err("KSW: failed to parse stack variable length\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * kstackwatch_proc_write - Handle writes to the /proc/kstackwatch file
 * @file: file struct for the proc entry
 * @buffer: user-space buffer containing the command string
 * @count: the number of bytes from the user-space buffer
 * @pos: file offset
 *
 * This function processes user input to control the kernel stack watcher
 * Before attempting to process any new configuration. It handles three
 * cases based on the input string, In all three cases, the system will
 * clear its state first, with subsequent actions determined by the input:
 *
 * 1. An empty or whitespace-only string
 *    Return a success code
 *
 * 2. An invalid configuration string
 *    Return a error code
 *
 * 3. A valid configuration string
 *    Start a new stack watch, return a success code
 *
 * Return: The number of bytes successfully processed on success,
 * or a negative error code on failure.
 */
static ssize_t kstackwatch_proc_write(struct file *file,
				      const char __user *buffer, size_t count,
				      loff_t *pos)
{
	char input[MAX_CONFIG_STR_LEN];
	int ret;

	if (count == 0 || count >= sizeof(input))
		return -EINVAL;

	if (copy_from_user(input, buffer, count))
		return -EFAULT;

	if (watching_active)
		ksw_stop_watching();
	memset(ksw_config, 0, sizeof(*ksw_config));

	input[count] = '\0';
	strim(input);

	/* case 1 */
	if (!strlen(input)) {
		pr_info("KSW: config cleared\n");
		return count;
	}

	ret = ksw_parse_config(input, ksw_config);
	if (ret) {
		/* case 2 */
		pr_err("KSW: Failed to parse config %d\n", ret);
		return ret;
	}

	/* case 3 */
	ret = ksw_start_watching();
	if (ret) {
		pr_err("KSW: Failed to start watching with %d\n", ret);
		return ret;
	}

	return count;
}

static int kstackwatch_proc_show(struct seq_file *m, void *v)
{
	if (watching_active) {
		seq_printf(m, "%s\n", ksw_config->config_str);
	} else {
		seq_puts(m, "not watching\n");
		seq_puts(m, "\nusage:\n");
		seq_puts(
			m,
			" echo 'function+ip_offset[+depth] [local_var_offset:local_var_len]' > /proc/kstackwatch\n");
		seq_puts(m, "  Watch the canary without the local var part.");
	}

	return 0;
}

static int kstackwatch_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, kstackwatch_proc_show, NULL);
}

static const struct proc_ops kstackwatch_proc_ops = {
	.proc_open = kstackwatch_proc_open,
	.proc_read = seq_read,
	.proc_write = kstackwatch_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init kstackwatch_init(void)
{
	ksw_config = kmalloc(sizeof(*ksw_config), GFP_KERNEL);
	if (!ksw_config)
		return -ENOMEM;

	/* Create proc interface */
	if (!proc_create("kstackwatch", 0644, NULL, &kstackwatch_proc_ops)) {
		pr_err("KSW: create proc kstackwatch fail");
		return -ENOMEM;
	}

	pr_info("KSW: module loaded\n");
	pr_info("KSW: usage:\n");
	pr_info("KSW: echo 'function+ip_offset[+depth] [local_var_offset:local_var_len]' > /proc/kstackwatch\n");

	return 0;
}

static void __exit kstackwatch_exit(void)
{
	/* Cleanup active watching */
	if (watching_active)
		ksw_stop_watching();

	/* Remove proc interface */
	remove_proc_entry("kstackwatch", NULL);
	kfree(ksw_config);

	pr_info("KSW: Module unloaded\n");
}

module_init(kstackwatch_init);
module_exit(kstackwatch_exit);
