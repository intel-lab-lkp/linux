// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 Lukas Hruska <lhruska@suse.cz>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>

const char *hello_msg = "Hello from";

static int hello_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s kernel module.\n", hello_msg);
}

static const struct kernel_param_ops hello_ops = {
	.get	= hello_get,
};

module_param_cb(hello, &hello_ops, NULL, 0400);
MODULE_PARM_DESC(hello, "Read only parameter greeting the reader.");

static int test_klp_extern_hello_init(void)
{
	return 0;
}

static void test_klp_extern_hello_exit(void)
{
}

module_init(test_klp_extern_hello_init);
module_exit(test_klp_extern_hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lukas Hruska <lhruska@suse.cz>");
MODULE_DESCRIPTION("Livepatch test: external symbol relocation - test module");
