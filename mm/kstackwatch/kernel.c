// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("Kernel Stack Watch");
MODULE_LICENSE("GPL");

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
