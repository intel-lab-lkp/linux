// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 Lukas Hruska <lhruska@suse.cz>

#define pr_fmt(fmt) "test_klp_extern_hello: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>

extern const char *hello_msg \
		   KLP_RELOC_SYMBOL(test_klp_extern_hello, test_klp_extern_hello, hello_msg);

static int hello_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s livepatched module.\n", hello_msg);
}

static struct klp_func funcs[] = {
	{
		.old_name = "hello_get",
		.new_func = hello_get,
	}, { }
};

static struct klp_object objs[] = {
	{
		.name = "test_klp_extern_hello",
		.funcs = funcs,
	}, { }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int test_klp_extern_init(void)
{
	return klp_enable_patch(&patch);
}

static void test_klp_extern_exit(void)
{
}

module_init(test_klp_extern_init);
module_exit(test_klp_extern_exit);
MODULE_LICENSE("GPL");
MODULE_INFO(livepatch, "Y");
MODULE_AUTHOR("Lukas Hruska <lhruska@suse.cz>");
MODULE_DESCRIPTION("Livepatch test: external symbol relocation");
