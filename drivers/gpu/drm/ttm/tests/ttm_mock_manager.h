/* SPDX-License-Identifier: GPL-2.0 AND MIT */
/*
 * Copyright © 2023 Intel Corporation
 */
#ifndef TTM_MOCK_MANAGER_H
#define TTM_MOCK_MANAGER_H

#include <drm/drm_buddy.h>

struct ttm_mock_manager {
	struct ttm_resource_manager man;
	struct drm_buddy mm;
	uint64_t default_page_size;
	/* protects allocations of mock buffer objects */
	struct mutex lock;
};

struct ttm_mock_resource {
	struct ttm_resource base;
	struct list_head blocks;
	unsigned long flags;
};

int ttm_mock_manager_init(struct ttm_device *bdev, uint32_t mem_type,
			  uint32_t size);
int ttm_bad_manager_init(struct ttm_device *bdev, uint32_t mem_type,
			 uint32_t size);
void ttm_mock_manager_fini(struct ttm_device *bdev, uint32_t mem_type);
void ttm_bad_manager_fini(struct ttm_device *bdev, uint32_t mem_type);

#endif // TTM_MOCK_MANAGER_H
