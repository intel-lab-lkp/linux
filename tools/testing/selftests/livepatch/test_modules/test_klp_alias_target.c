// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Harry Hsu <x90613@gmail.com>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static struct proc_dir_entry *pde;

static noinline int test_klp_alias_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s: %s\n", THIS_MODULE->name, "original output");
	return 0;
}

/*
 * Alias the function above so that both names resolve to one address, the
 * way __do_sys_fork(), __ia32_sys_fork() and __x64_sys_fork() do in vmlinux.
 * Nothing calls the alias, it only has to show up in the module's symbol
 * table for the livepatch to name it.
 */
static int test_klp_alias_show_alias(struct seq_file *m, void *v)
	__used __alias(test_klp_alias_show);

static int test_klp_alias_target_init(void)
{
	pr_info("%s\n", __func__);
	pde = proc_create_single("test_klp_alias_target", 0, NULL,
				 test_klp_alias_show);
	if (!pde)
		return -ENOMEM;
	return 0;
}

static void test_klp_alias_target_exit(void)
{
	pr_info("%s\n", __func__);
	proc_remove(pde);
}

module_init(test_klp_alias_target_init);
module_exit(test_klp_alias_target_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harry Hsu <x90613@gmail.com>");
MODULE_DESCRIPTION("Livepatch test: target module with two aliased symbols");
