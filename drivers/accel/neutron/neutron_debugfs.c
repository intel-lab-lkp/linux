// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2025 NXP */

#include <linux/debugfs.h>

#include "neutron_device.h"
#include "neutron_debugfs.h"

static ssize_t fw_log_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
	struct neutron_device *ndev = file_inode(f)->i_private;

	if (!ndev->log.size)
		return 0;

	if (ndev->flags & NEUTRON_BOOTED)
		neutron_read_log(ndev, count);

	return simple_read_from_buffer(buf, count, pos, ndev->log.buf,
				       ndev->log.buf_count);
}

static const struct file_operations fw_log_fops = {
	.owner = THIS_MODULE,
	.read = fw_log_read,
};

void neutron_debugfs_init(struct neutron_device *ndev)
{
	struct dentry *debugfs_root;

	debugfs_root = ndev->base.debugfs_root;
	debugfs_create_file("fw_log", 0444, debugfs_root, ndev, &fw_log_fops);
}
