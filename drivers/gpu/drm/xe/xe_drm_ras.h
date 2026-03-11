/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */
#ifndef XE_DRM_RAS_H_
#define XE_DRM_RAS_H_

#include <linux/types.h>

#include <drm/xe_drm.h>

struct xe_device;
struct xe_drm_ras;

#define for_each_error_severity(i)	\
	for (i = 0; i < DRM_XE_RAS_ERR_SEV_MAX; i++)

int xe_drm_ras_init(struct xe_device *xe);
void xe_drm_ras_notify(struct xe_drm_ras *ras, u32 error_id,
		       const enum drm_xe_ras_error_severity severity, gfp_t flags);

#endif
