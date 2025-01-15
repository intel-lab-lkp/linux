// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/printk.h>

/**
 * test_klp_speaker - test module for testing misc livepatching features
 *
 * The module provides a virtual speaker who can do:
 *
 *    - Start a show with a greeting, see speaker_welcome().
 *
 *    - Log the greeting by reading the "welcome" module parameter, see
 *	welcome_get().
 */

noinline
static void speaker_welcome(void)
{
	pr_info("%s: Hello, World!\n", __func__);
}

static int welcome_get(char *buffer, const struct kernel_param *kp)
{
	speaker_welcome();

	return 0;
}

static const struct kernel_param_ops welcome_ops = {
	.get	= welcome_get,
};

module_param_cb(welcome, &welcome_ops, NULL, 0400);
MODULE_PARM_DESC(welcome, "Print speaker's welcome message into the kernel log when reading the value.");

static int test_klp_speaker_init(void)
{
	pr_info("%s\n", __func__);

	return 0;
}

static void test_klp_speaker_exit(void)
{
	pr_info("%s\n", __func__);
}

module_init(test_klp_speaker_init);
module_exit(test_klp_speaker_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Livepatch test: test functions");
