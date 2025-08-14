// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, hisilion limited.
 * Debug helper to dump the current IO pagetables of the system
 * so that we can see what the various memory ranges are set to.
 *
 * Author: Qinxin Xia <xiaqinxin@huawei.com>
 */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/init.h>
#include <linux/io_ptdump.h>
#include <linux/mm.h>
#include <linux/seq_file.h>

static int __init io_ptdump_init(void)
{
	io_ptdump_debugfs_register("io_page_tables");
	return 0;
}

device_initcall(io_ptdump_init);
