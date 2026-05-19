/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTM_VRAM_MGR_TYPES_H_
#define _XE_TTM_VRAM_MGR_TYPES_H_

#include <linux/gpu_buddy.h>
#include <drm/ttm/ttm_device.h>

/**
 * struct xe_ttm_vram_mgr - Xe TTM VRAM manager
 *
 * Manages placement of TTM resource in VRAM.
 */
struct xe_ttm_vram_mgr {
	/** @manager: Base TTM resource manager */
	struct ttm_resource_manager manager;
	/** @mm: DRM buddy allocator which manages the VRAM */
	struct gpu_buddy mm;
	/** @offlined_pages: List of offlined pages */
	struct list_head offlined_pages;
	/** @n_offlined_pages: Number of offlined pages */
	u16 n_offlined_pages;
	/** @queued_pages: List of queued pages */
	struct list_head queued_pages;
	/** @n_queued_pages: Number of queued pages */
	u16 n_queued_pages;
	/** @visible_size: Proped size of the CPU visible portion */
	u64 visible_size;
	/** @visible_avail: CPU visible portion still unallocated */
	u64 visible_avail;
	/** @default_page_size: default page size */
	u64 default_page_size;
	/** @lock: protects allocations of VRAM */
	struct mutex lock;
	/** @mem_type: The TTM memory type */
	u32 mem_type;
};

/**
 * struct xe_ttm_vram_mgr_resource - Xe TTM VRAM resource
 */
struct xe_ttm_vram_mgr_resource {
	/** @base: Base TTM resource */
	struct ttm_resource base;
	/** @blocks: list of DRM buddy blocks */
	struct list_head blocks;
	/** @used_visible_size: How many CPU visible bytes this resource is using */
	u64 used_visible_size;
	/** @flags: flags associated with the resource */
	unsigned long flags;
};

/**
 * struct xe_ttm_vram_offline_resource - Xe TTM VRAM offline  resource
 */
struct xe_ttm_vram_offline_resource {
	/** @offlined_link: Link to offlined pages */
	struct list_head offlined_link;
	/** @queued_link: Link to queued pages */
	struct list_head queued_link;
	/** @blocks: list of DRM buddy blocks */
	struct list_head blocks;
	/** @used_visible_size: How many CPU visible bytes this resource is using */
	u64 used_visible_size;
	/** @id: The id of an offline resource */
	u16 id;
	/** @addr: Address of faulty memory location reported by HW */
	unsigned long addr;
	/** @status: reservation status of resource */
	bool status;
};

#endif
