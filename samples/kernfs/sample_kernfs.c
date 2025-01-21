// SPDX-License-Identifier: GPL-2.0-only
/*
 * A sample kernel module showing how to build a pseudo-filesystem on top of
 * kernfs.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>

static int __init sample_kernfs_init(void)
{
	pr_info("Loaded sample_kernfs module.\n");
	return 0;
}

module_init(sample_kernfs_init)
MODULE_DESCRIPTION("Sample kernel module showing how to use kernfs");
MODULE_LICENSE("GPL");
