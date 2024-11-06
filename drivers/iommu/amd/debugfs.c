// SPDX-License-Identifier: GPL-2.0
/*
 * AMD IOMMU driver
 *
 * Copyright (C) 2018 Advanced Micro Devices, Inc.
 *
 * Author: Gary R Hook <gary.hook@amd.com>
 */

#include <linux/debugfs.h>
#include <linux/pci.h>

#include "amd_iommu.h"

static struct dentry *amd_iommu_debugfs;

#define	MAX_NAME_LEN	20
#define	OFS_IN_SZ	8

static int mmio_offset = -1;

static ssize_t iommu_mmio_write(struct file *filp, const char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	struct seq_file *m = filp->private_data;
	struct amd_iommu *iommu = m->private;
	int ret;

	if (cnt > OFS_IN_SZ)
		return -EINVAL;

	ret = kstrtou32_from_user(ubuf, cnt, 0, &mmio_offset);
	if (ret)
		return ret;

	if (mmio_offset > iommu->mmio_phys_end - 4) {
		mmio_offset = -1;
		return  -EINVAL;
	}

	return cnt;
}

static int iommu_mmio_show(struct seq_file *m, void *unused)
{
	if (mmio_offset >= 0)
		seq_printf(m, "0x%x\n", mmio_offset);
	else
		seq_puts(m, "No or invalid input provided\n");

	return 0;
}
DEFINE_SHOW_STORE_ATTRIBUTE(iommu_mmio);

static int iommu_mmio_dump_show(struct seq_file *m, void *unused)
{
	struct amd_iommu *iommu = m->private;
	u32 value;

	if (mmio_offset < 0) {
		seq_puts(m, "Please provide mmio register's offset\n");
		return 0;
	}

	value = readl(iommu->mmio_base + mmio_offset);
	seq_printf(m, "0x%08x\n", value);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(iommu_mmio_dump);

void amd_iommu_debugfs_setup(void)
{
	struct amd_iommu *iommu;
	char name[MAX_NAME_LEN + 1];

	amd_iommu_debugfs = debugfs_create_dir("amd", iommu_debugfs_dir);

	for_each_iommu(iommu) {
		snprintf(name, MAX_NAME_LEN, "iommu%02d", iommu->index);
		iommu->debugfs = debugfs_create_dir(name, amd_iommu_debugfs);

		debugfs_create_file("mmio", 0644, iommu->debugfs, iommu,
				    &iommu_mmio_fops);
		debugfs_create_file("mmio_dump", 0444, iommu->debugfs, iommu,
				    &iommu_mmio_dump_fops);
	}
}
