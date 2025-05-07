// SPDX-License-Identifier: GPL-2.0
/*
 * hfs debug support
 *
 * Copyright (c) 2025 Yangtao Li <frank.li@vivo.com>
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "hfs_fs.h"

#if IS_ENABLED(CONFIG_DEBUG_FS)
static struct dentry *hfs_debugfs_root;
u8 dbg_flags;

void __init hfs_debug_init(void)
{
	hfs_debugfs_root = debugfs_create_dir("hfs", NULL);
	debugfs_create_u8("dbg_flags", 0600, hfs_debugfs_root, &dbg_flags);
}

void hfs_debug_exit(void)
{
	debugfs_remove_recursive(hfs_debugfs_root);
}
#else
void __init hfs_debug_init(void) {}
void hfs_debug_exit(void) {}
#endif
