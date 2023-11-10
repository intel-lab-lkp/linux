// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2023 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifndef _VER_NAME
#define _VER_NAME(name) name
#endif

#include <linux/module.h>
#include <linux/printk.h>

noinline
void speaker_welcome(void)
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
