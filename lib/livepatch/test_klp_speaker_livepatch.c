// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/init.h>


void livepatch_speaker_welcome(void)
{
	pr_info("%s: Ladies and gentleman, ...\n", __func__);
}


static struct klp_func test_klp_speaker_funcs[] = {
	{
		.old_name = "speaker_welcome",
		.new_func = livepatch_speaker_welcome,
	},
	{ }
};

static struct klp_object objs[] = {
	{
		.name = "test_klp_speaker",
		.funcs = test_klp_speaker_funcs,
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
