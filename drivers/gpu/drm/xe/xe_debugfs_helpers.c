// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>

#include <linux/debugfs.h>

#include "xe_debugfs_helpers.h"

static int xe_debugfs_open(struct inode *inode, struct file *file)
{
	struct drm_info_node *node = inode->i_private;

	return single_open(file, node->info_ent->show, node);
}

static const struct file_operations xe_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = xe_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * xe_debugfs_create_files - Initialize a given set of debugfs files.
 * @files: The array of files to create.
 * @count: The number of files given.
 * @root: debugfs dir root entry.
 * @xe: the &xe_device to register
 *
 * Create a given set of debugfs files represented by an array of
 * &struct drm_info_list in the given root directory. These files will be
 * removed automatically on drm_debugfs_dev_fini().
 *
 * Returns 0 on success, negative error code on failure.
 */
int xe_debugfs_create_files(const struct drm_info_list *files, int count,
			    struct dentry *root, struct xe_device *xe)
{
	struct drm_device *dev = &xe->drm;
	struct drm_info_node *tmp;
	struct drm_minor *minor;
	int i;

	if (!xe_device_is_admin_only(xe)) {
		minor = dev->primary;
		drm_debugfs_create_files(files, count, root, minor);
		return 0;
	}

	for (i = 0; i < count; i++) {
		u32 features = files[i].driver_features;

		if (features && !drm_core_check_all_features(dev, features))
			continue;

		tmp = drmm_kzalloc(dev, sizeof(*tmp), GFP_KERNEL);
		if (!tmp)
			return -ENOMEM;

		tmp->minor = drmm_kzalloc(dev, sizeof(*tmp->minor), GFP_KERNEL);
		if (!tmp->minor)
			return -ENOMEM;

		tmp->minor->dev = dev;
		tmp->dent = debugfs_create_file(files[i].name,
						0444, root, tmp,
						&xe_debugfs_fops);
		tmp->info_ent = &files[i];
	}

	return 0;
}
