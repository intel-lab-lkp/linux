// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual swap space
 *
 * Copyright (C) 2024 Meta Platforms, Inc., Nhat Pham
 */
#include <linux/swap.h>

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static struct dentry *vswap_debugfs_root;

static int vswap_debug_fs_init(void)
{
	if (!debugfs_initialized())
		return -ENODEV;

	vswap_debugfs_root = debugfs_create_dir("vswap", NULL);
	return 0;
}
#else
static int vswap_debug_fs_init(void)
{
	return 0;
}
#endif

int vswap_init(void)
{
	if (vswap_debug_fs_init())
		pr_warn("Failed to initialize vswap debugfs\n");

	return 0;
}
