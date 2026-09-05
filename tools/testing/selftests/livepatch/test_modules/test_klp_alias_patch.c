// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Harry Hsu <x90613@gmail.com>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/seq_file.h>

static int livepatch_alias_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s: %s\n", THIS_MODULE->name,
		   "this has been live patched");
	return 0;
}

/*
 * Both names resolve to one address, so they end up on a single
 * ops->func_stack and the redirection would be ambiguous.  Loading this
 * livepatch is expected to fail.
 */
static struct klp_func funcs[] = {
	{
		.old_name = "test_klp_alias_show",
		.new_func = livepatch_alias_show,
	},
	{
		.old_name = "test_klp_alias_show_alias",
		.new_func = livepatch_alias_show,
	},
	{},
};

static struct klp_object objs[] = {
	{
		.name = "test_klp_alias_target",
		.funcs = funcs,
	},
	{},
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int test_klp_alias_patch_init(void)
{
	return klp_enable_patch(&patch);
}

static void test_klp_alias_patch_exit(void)
{
}

module_init(test_klp_alias_patch_init);
module_exit(test_klp_alias_patch_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
MODULE_AUTHOR("Harry Hsu <x90613@gmail.com>");
MODULE_DESCRIPTION("Livepatch test: patch two aliased symbols of one object");
