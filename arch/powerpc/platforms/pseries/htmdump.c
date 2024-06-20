// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) IBM Corporation, 2024
 */

#define pr_fmt(fmt) "htmdump: " fmt

#include <linux/bitops.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/numa.h>
#include <linux/memblock.h>
#include <asm/machdep.h>
#include <asm/plpar_wrappers.h>

/* This enables us to keep track of the memory removed from each node. */
struct htmdump_entry {
	void *buf;
	struct dentry *dir;
	char name[16];
};

static u32 nodeindex = 0;
static u32 nodalchipindex = 0;
static u32 coreindexonchip = 0;
static u32 htmtype = 0;

#define BUFFER_SIZE PAGE_SIZE

static ssize_t htmdump_read(struct file *filp, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct htmdump_entry *ent = filp->private_data;
	unsigned long page, read_size, available;
	loff_t offset;
	long rc;

	page = ALIGN_DOWN(*ppos, BUFFER_SIZE);
	offset = (*ppos) % BUFFER_SIZE;

	rc = htm_get_dump_hardware(nodeindex, nodalchipindex, coreindexonchip,
				   htmtype, virt_to_phys(ent->buf), BUFFER_SIZE, page);

	switch(rc) {
	case H_SUCCESS:
	case H_PARTIAL:
		break;
	case H_NOT_AVAILABLE:
		return 0;
	case H_BUSY:
	case H_LONG_BUSY_ORDER_1_MSEC:
	case H_LONG_BUSY_ORDER_10_MSEC:
	case H_LONG_BUSY_ORDER_100_MSEC:
	case H_LONG_BUSY_ORDER_1_SEC:
	case H_LONG_BUSY_ORDER_10_SEC:
	case H_LONG_BUSY_ORDER_100_SEC:
	case H_PARAMETER:
	case H_P2:
	case H_P3:
	case H_P4:
	case H_P5:
	case H_P6:
	case H_STATE:
	case H_AUTHORITY:
		return -EINVAL;
	}

	available = BUFFER_SIZE - offset;
	read_size = min(count, available);
	*ppos += read_size;
	return simple_read_from_buffer(ubuf, count, &offset, ent->buf, available);
}

static const struct file_operations htmdump_fops = {
	.llseek = default_llseek,
	.read	= htmdump_read,
	.open	= simple_open,
};

static struct dentry *htmdump_debugfs_dir;

static int htmdump_init_debugfs(void)
{
	struct htmdump_entry *ent;

	ent = kcalloc(1, sizeof(struct htmdump_entry), GFP_KERNEL);
	if (!ent) {
		pr_err("Failed to allocate ent\n");
		return -EINVAL;
	}

	ent->buf = kmalloc(BUFFER_SIZE, GFP_KERNEL);
	if (!ent->buf) {
		pr_err("Failed to allocate htmdump buf\n");
		return -ENOMEM;
	}

	pr_debug("%s: ent:%lx buf:%lx\n",
			__func__, (long unsigned int)ent, (long unsigned int)ent->buf);

	htmdump_debugfs_dir = debugfs_create_dir("htmdump",
						  arch_debugfs_dir);

	debugfs_create_u32("nodeindex", 0600,
			htmdump_debugfs_dir, &nodeindex);
	debugfs_create_u32("nodalchipindex", 0600,
			htmdump_debugfs_dir, &nodalchipindex);
	debugfs_create_u32("coreindexonchip", 0600,
			htmdump_debugfs_dir, &coreindexonchip);
	debugfs_create_u32("htmtype", 0600,
			htmdump_debugfs_dir, &htmtype);
	debugfs_create_file("trace", 0400, htmdump_debugfs_dir, ent, &htmdump_fops);

	return 0;
}

static int htmdump_init(void)
{
	if (htmdump_init_debugfs())
		return -EINVAL;

	return 0;
}
machine_device_initcall(pseries, htmdump_init);
