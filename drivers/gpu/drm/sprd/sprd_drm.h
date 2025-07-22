/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Unisoc Inc.
 */

#ifndef _SPRD_DRM_H_
#define _SPRD_DRM_H_

#include <drm/drm_atomic.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>

struct sprd_drm {
	struct drm_device drm;

	struct iommu_domain *iommu_domain;
	struct mutex mm_lock;
	struct drm_mm mm;
};

#define to_sprd_drm(x)	container_of(x, struct sprd_drm, drm)

int sprd_drm_iommu_attach(struct drm_device *drm, struct device *dpu_dev);
void sprd_drm_iommu_detach(struct drm_device *drm, struct device *dpu_dev);

extern struct platform_driver sprd_dpu_driver;
extern struct platform_driver sprd_dsi_driver;

#endif /* _SPRD_DRM_H_ */
