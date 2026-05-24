// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Pablo Hugen <phugen@redhat.com>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/seq_file.h>

static int livepatch_mod_target_get(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%s: %s\n", THIS_MODULE->name,
		       "this has been live patched");
}

static struct klp_func funcs[] = {
	{
		.old_name = "test_klp_mod_target_get",
		.new_func = livepatch_mod_target_get,
	},
	{},
};

static struct klp_object objs[] = {
	{
		.name = "test_klp_mod_target",
		.funcs = funcs,
	},
	{},
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int test_klp_mod_patch_init(void)
{
	return klp_enable_patch(&patch);
}

static void test_klp_mod_patch_exit(void)
{
}

module_init(test_klp_mod_patch_init);
module_exit(test_klp_mod_patch_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
MODULE_AUTHOR("Pablo Hugen <phugen@redhat.com>");
MODULE_DESCRIPTION("Livepatch test: patch for module-provided function");
