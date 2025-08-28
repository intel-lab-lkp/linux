// SPDX-License-Identifier: GPL-2.0
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/string.h>

#include "kstackwatch.h"

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("Kernel Stack Watch");
MODULE_LICENSE("GPL");

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

static int __init kstackwatch_init(void)
{
	pr_info("KSW: module loaded\n");
	pr_info("KSW: usage:\n");
	pr_info("KSW: echo 'function+ip_offset[+depth] [local_var_offset:local_var_len]' > /proc/kstackwatch\n");

	return 0;
}

static void __exit kstackwatch_exit(void)
{
	pr_info("KSW: Module unloaded\n");
}

module_init(kstackwatch_init);
module_exit(kstackwatch_exit);
