// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 SUSE

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/init.h>

/**
 * test_klp_speaker_livepatch - test livepatch for testing various livepatching
 *	features.
 *
 * The livepatch modifies the behavior of a virtual speaker provided by
 * the module test_klp_speaker. It can do:
 *
 *    - Improve the speaker's greeting from "Hello, World!" to
 *	"Ladies and gentleman, ..."
 *
 *    - Support more speaker modules, see __lp_speaker_welcome().
 */

static void __lp_speaker_welcome(const char *caller_func, const char *speaker_id)
{
	pr_info("%s%s: Ladies and gentleman, ...\n", caller_func, speaker_id);
}

static void lp_speaker_welcome(void)
{
	__lp_speaker_welcome(__func__, "");
}

static void lp_speaker2_welcome(void)
{
	__lp_speaker_welcome(__func__, "(2)");
}

static struct klp_func test_klp_speaker_funcs[] = {
	{
		.old_name = "speaker_welcome",
		.new_func = lp_speaker_welcome,
	},
	{ }
};

static struct klp_func test_klp_speaker2_funcs[] = {
	{
		.old_name = "speaker_welcome",
		.new_func = lp_speaker2_welcome,
	},
	{ }
};

static struct klp_object objs[] = {
	{
		.name = "test_klp_speaker",
		.funcs = test_klp_speaker_funcs,
	},
	{
		.name = "test_klp_speaker2",
		.funcs = test_klp_speaker2_funcs,
	},
	{ }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int test_klp_speaker_livepatch_init(void)
{
	return klp_enable_patch(&patch);
}

static void test_klp_speaker_livepatch_exit(void)
{
}

module_init(test_klp_speaker_livepatch_init);
module_exit(test_klp_speaker_livepatch_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
MODULE_DESCRIPTION("Livepatch test: livepatch test_klp_speaker test module");
