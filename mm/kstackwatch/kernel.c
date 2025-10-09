// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>

static int __init kstackwatch_init(void)
{
	pr_info("module loaded\n");
	return 0;
}

static void __exit kstackwatch_exit(void)
{
	pr_info("module unloaded\n");
}

module_init(kstackwatch_init);
module_exit(kstackwatch_exit);

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("Kernel Stack Watch");
MODULE_LICENSE("GPL");

