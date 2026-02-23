/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026-2027 Intel Corporation
 */

#ifndef _XE_DEBUGFS_HELPERS_H_
#define _XE_DEBUGFS_HELPERS_H_

#include "xe_gt_types.h"
#include "xe_sriov.h"

struct drm_info_list;

int xe_debugfs_create_files(const struct drm_info_list *files, int count,
			    struct dentry *root, struct xe_device *xe);

static inline struct dentry *xe_debugfs_root_dir(struct xe_device *xe)
{
	struct drm_minor *minor = xe->drm.primary;

	if (xe_device_is_admin_only(xe))
		return xe->drm.debugfs_root;
	else
		return minor->debugfs_root;
}
#endif

