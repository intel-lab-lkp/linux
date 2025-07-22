/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 Unisoc Inc.
 */

#ifndef _SPRD_GEM_H_
#define _SPRD_GEM_H_

#include <drm/drm_gem.h>

struct sprd_gem_obj {
	struct drm_gem_object	base;
	dma_addr_t		dma_addr;
	struct sg_table		*sgt;
	void			*vaddr;

	/* Used when IOMMU is enabled */
	struct drm_mm_node	mm;
	struct page		**pages;
	size_t			map_size;
};

#define to_sprd_gem_obj(x)	container_of(x, struct sprd_gem_obj, base)

int sprd_gem_dumb_create(struct drm_file *file_priv,
			 struct drm_device *dev,
			 struct drm_mode_create_dumb *args);

struct drm_gem_object *
sprd_gem_prime_import_sg_table(struct drm_device *dev,
			       struct dma_buf_attachment *attach,
			       struct sg_table *sgtb);

#endif
