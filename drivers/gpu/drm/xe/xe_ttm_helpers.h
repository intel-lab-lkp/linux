/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _XE_TTM_HELPERS_H_
#define _XE_TTM_HELPERS_H_

#include <linux/types.h>

struct drm_exec;
struct ttm_device;
struct ttm_buffer_object;
struct ttm_operation_ctx;
struct ttm_resource_manager;

struct xe_ttm_lru_walk;

/** struct xe_ttm_lru_walk_ops - Operations for a LRU walk. */
struct xe_ttm_lru_walk_ops {
	/**
	 * allow_bo - Allow calling @process_bo for this bo.
	 * @walk: struct xe_ttm_lru_walk describing the walk.
	 * @bo: A locked and referenced buffer object.
	 * @mem_type: The memory type of the LRU list.
	 *
	 * RFC: Combine these two functions?
	 *
	 * Return: true if @process_bo should be called, @false otherwise.
	 */
	bool (*allow_bo)(struct xe_ttm_lru_walk *walk, struct ttm_buffer_object *bo,
			 unsigned int mem_type);

	/**
	 * process_bo - Process this bo.
	 * @walk: struct xe_ttm_lru_walk describing the walk.
	 * @bo: A locked and referenced buffer object.
	 *
	 * Return: Negative error code on error, Number of processed pages on
	 * success. 0 also indicates success.
	 */
	long (*process_bo)(struct xe_ttm_lru_walk *walk, struct ttm_buffer_object *bo);
};

/**
 * struct xe_ttm_lru_walk - Structure describing a LRU walk.
 * @ops: Pointer to the ops structure.
 * @ctx: Pointer to the struct ttm_operation_ctx.
 * @exec: The struct drm_exec context for the WW transaction if any.
 */
struct xe_ttm_lru_walk {
	const struct xe_ttm_lru_walk_ops *ops;
	struct ttm_operation_ctx *ctx;
	struct drm_exec *exec;
};

long xe_ttm_bo_try_shrink(struct xe_ttm_lru_walk *walk, struct ttm_buffer_object *bo,
			  bool purge);

long xe_ttm_lru_walk_for_evict(struct xe_ttm_lru_walk *walk, struct ttm_device *bdev,
			       struct ttm_resource_manager *man, unsigned int mem_type,
			       long target);
#endif
