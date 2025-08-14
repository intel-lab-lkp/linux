// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/iommu.h>
#include <linux/memory_hotplug.h>
#include <linux/seq_file.h>

static int io_ptdump_show(struct seq_file *m, void *v)
{
	iommu_group_and_iova_dump(m);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(io_ptdump);

void __init io_ptdump_debugfs_register(const char *name)
{
	debugfs_create_file(name, 0400, NULL, NULL, &io_ptdump_fops);
}
