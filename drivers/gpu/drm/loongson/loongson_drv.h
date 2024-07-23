/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Authors:
 *      Sui Jingfeng <sui.jingfeng@linux.dev>
 */

#ifndef __LOONGSON_DRV_H__
#define __LOONGSON_DRV_H__

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/ttm/ttm_device.h>

#include "loongson_module.h"
#include "lsdc_gem.h"
#include "lsdc_gfxpll.h"

struct loongson_drm {
	struct drm_device ddev;

	struct device *dev;

	/* submodules */
	struct lsdc_device *lsdc;
	struct loonggpu_device *loonggpu;

	const struct loongson_gfx_desc *gfxinfo;

	struct loongson_gfxpll gfxpll;

	/* memory manager */
	struct ttm_device bdev;
	resource_size_t vram_base;
	resource_size_t vram_size;
	resource_size_t gtt_base;
	resource_size_t gtt_size;

	struct loongson_gem gem;

	/* tracking pinned memory */
	size_t vram_pinned_size;
	size_t gtt_pinned_size;
};

static inline struct loongson_drm *
to_loongson_drm(struct drm_device *drm)
{
	return container_of(drm, struct loongson_drm, ddev);
}

static inline struct lsdc_device *
to_lsdc(struct drm_device *drm)
{
	return to_loongson_drm(drm)->lsdc;
}

static inline struct loonggpu_device *
to_loongpu(struct drm_device *drm)
{
	return to_loongson_drm(drm)->loonggpu;
}

static inline resource_size_t
loongson_drm_vram_base(struct drm_device *drm)
{
	return to_loongson_drm(drm)->vram_base;
}

static inline resource_size_t
loongson_drm_vram_size(struct drm_device *drm)
{
	return to_loongson_drm(drm)->vram_size;
}

static inline struct loongson_gem *
to_loongson_gem(struct drm_device *drm)
{
	return &(to_loongson_drm(drm)->gem);
}

static inline struct loongson_drm *
tdev_to_ldrm(struct ttm_device *tdev)
{
	return container_of(tdev, struct loongson_drm, bdev);
}

int loongson_device_preinit(struct device *parent);
void loongson_debugfs_init(struct drm_minor *minor);

#endif
