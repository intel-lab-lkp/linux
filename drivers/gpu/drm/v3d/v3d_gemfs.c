// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2024 Raspberry Pi */

#include "v3d_drv.h"

void v3d_gemfs_init(struct v3d_dev *v3d)
{
	struct vfsmount *gemfs;

	/*
	 * By creating our own shmemfs mountpoint, we can pass in
	 * mount flags that better match our usecase. However, we
	 * only do so on platforms which benefit from it.
	 */
	if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
		goto err;

	/* The user doesn't want to enable Super Pages */
	if (!super_pages)
		goto err;

	gemfs = drm_gem_shmem_huge_mnt_create("within_size");
	if (IS_ERR(gemfs))
		goto err;

	v3d->gemfs = gemfs;
	drm_info(&v3d->drm, "Using Transparent Hugepages\n");

	return;

err:
	v3d->gemfs = NULL;
	drm_notice(&v3d->drm,
		   "Transparent Hugepage support is recommended for optimal performance on this platform!\n");
}

void v3d_gemfs_fini(struct v3d_dev *v3d)
{
	drm_gem_shmem_huge_mnt_free(v3d->gemfs);
}
