// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Pablo Hugen <phugen@redhat.com>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>

static noinline int test_klp_mod_target_get(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%s: %s\n", THIS_MODULE->name, "original output");
}

static const struct kernel_param_ops test_klp_mod_target_ops = {
	.get = test_klp_mod_target_get,
};

module_param_cb(klp_mod_arg, &test_klp_mod_target_ops, NULL, 0444);
MODULE_PARM_DESC(klp_mod_arg, "The value of this argument will be livepatched");

static int test_klp_mod_target_init(void)
{
	pr_info("%s\n", __func__);
	return 0;
}

static void test_klp_mod_target_exit(void)
{
	pr_info("%s\n", __func__);
}

module_init(test_klp_mod_target_init);
module_exit(test_klp_mod_target_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pablo Hugen <phugen@redhat.com>");
MODULE_DESCRIPTION("Livepatch test: target module with proc entry");
