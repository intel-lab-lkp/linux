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
 */

noinline
static void __always_used speaker_welcome(void)
{
	pr_info("%s: Hello, World!\n", __func__);
}

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
