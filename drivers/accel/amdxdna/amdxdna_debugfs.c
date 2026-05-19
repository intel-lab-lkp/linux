// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024, Advanced Micro Devices, Inc.
 */

#include <drm/drm_debugfs.h>
#include <drm/drm_device.h>

#include "amdxdna_debugfs.h"
#include "amdxdna_pci_drv.h"

static inline struct amdxdna_dev *seq_to_xdna(struct seq_file *s)
{
	struct drm_debugfs_entry *entry = s->private;

	return to_xdna_dev(entry->dev);
}

static int fw_version_show(struct seq_file *s, void *v)
{
	struct amdxdna_dev *xdna = seq_to_xdna(s);

	seq_printf(s, "%d.%d.%d.%d\n",
		   xdna->fw_ver.major, xdna->fw_ver.minor,
		   xdna->fw_ver.sub, xdna->fw_ver.build);
	return 0;
}

static int device_info_show(struct seq_file *s, void *v)
{
	struct amdxdna_dev *xdna = seq_to_xdna(s);

	seq_printf(s, "vbnv:        %s\n", xdna->dev_info->vbnv);
	seq_printf(s, "device_type: %d\n", xdna->dev_info->device_type);
	return 0;
}

static int clients_show(struct seq_file *s, void *v)
{
	struct amdxdna_dev *xdna = seq_to_xdna(s);
	struct amdxdna_client *client;

	mutex_lock(&xdna->dev_lock);
	list_for_each_entry(client, &xdna->client_list, node)
		seq_printf(s, "pid %d heap_usage %zu total_bo_usage %zu\n",
			   client->pid, client->heap_usage,
			   client->total_bo_usage);
	mutex_unlock(&xdna->dev_lock);

	return 0;
}

static const struct drm_debugfs_info amdxdna_debugfs_list[] = {
	{ "fw_version",  fw_version_show,  0 },
	{ "device_info", device_info_show, 0 },
	{ "clients",     clients_show,     0 },
};

void amdxdna_debugfs_init(struct amdxdna_dev *xdna)
{
	drm_debugfs_add_files(&xdna->ddev, amdxdna_debugfs_list,
			      ARRAY_SIZE(amdxdna_debugfs_list));
}
