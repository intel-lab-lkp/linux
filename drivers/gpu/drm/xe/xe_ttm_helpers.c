// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/drm_exec.h>

#include "xe_ttm_helpers.h"

#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>

static bool xe_ttm_lru_walk_trylock(struct xe_ttm_lru_walk *walk,
				    struct ttm_buffer_object *bo,
				    bool *needs_unlock)
{
	struct ttm_operation_ctx *ctx = walk->ctx;

	*needs_unlock = false;

	if (!walk->exec && dma_resv_trylock(bo->base.resv)) {
		*needs_unlock = true;
		return true;
	}

	if (bo->base.resv == ctx->resv && ctx->allow_res_evict) {
		dma_resv_assert_held(bo->base.resv);
		return true;
	}

	return false;
}

static void xe_ttm_lru_walk_unlock(struct ttm_buffer_object *bo, bool locked)
{
	if (locked)
		dma_resv_unlock(bo->base.resv);
}

/**
 * xe_ttm_lru_walk_for_evict() - Perform a LRU list walk, with actions taken on
 * valid items.
 * @walk: describe the walks and actions taken
 * @bdev: The TTM device.
 * @man: The struct ttm_resource manager whose LRU lists we're walking.
 * @mem_type: The memory type associated with @man.
 * @target: The end condition for the walk.
 *
 * The LRU lists of @man are walk, and for each struct ttm_resource encountered,
 * the corresponding ttm_buffer_object is locked and taken a reference on, and
 * the LRU lock is dropped. the LRU lock may be dropped before locking and, in
 * that case, it's verified that the item actually remains on the LRU list after
 * the lock, and that the buffer object hasn't changed.
 *
 * With a locked object, the actions indicated by @walk->allow_bo() and
 * @walk->process_bo are performed. (RFC: Combine these?), and after that, the
 * bo is unlocked, the refcount dropped and the next struct ttm_resource is
 * processed. Here we rely on TTM's restartable LRU list implementation.
 *
 * Typically @walk->process_bo() would return the number of pages evicted, and
 * that when the total exceeds @target, or when the LRU list has been walked
 * in full, iteration is terminated. It's also terminated on error.
 *
 * Buffer object dma_resv locking:
 * This locking is performed using the combined interpretation of @walk->exec and
 * @walk->ctx according to the following.
 * 1) Sleeping locks: Sleeping locks are used exclusively if @walk->exec is true.
 * The buffer object are not unlocked. That is the caller's responsibility.
 * 2) Assuming bo is already locked: This assumption is made iff @walk->exec is false,
 * @walk->ctx->allow_res_evict is true and bo->base.resv == @walk->ctx->resv.
 * This is for cases where it is desired to evict bos sharing a reservation lock
 * that is already held by the process. Thes bo locks are not unlocked during
 * the walk.
 * 3) Trylocking. Trylocking is done in all other cases. If trylocking fails, the
 * iteration skips the current item and continues. Trylocks are always unlocked
 * by the walk.
 *
 * Note that the way dma_resv individualization is done, locking needs to be done
 * either with the LRU lock held (trylocking only) or with a reference on the
 * object.
 *
 * Return: (Typically) The number of pages evicted or negative error code on error.
 */
long xe_ttm_lru_walk_for_evict(struct xe_ttm_lru_walk *walk, struct ttm_device *bdev,
			       struct ttm_resource_manager *man, unsigned int mem_type,
			       long target)
{
	struct drm_exec *exec = walk->exec;
	struct ttm_resource_cursor cursor;
	struct ttm_resource *res;
	long sofar = 0;
	long lret;
	int ret;

	spin_lock(&bdev->lru_lock);
	ttm_resource_manager_for_each_res(man, &cursor, res) {
		struct ttm_buffer_object *bo = res->bo;
		bool bo_needs_unlock = false;
		bool bo_locked = false;

		if (!bo || bo->resource != res)
			continue;

		if (xe_ttm_lru_walk_trylock(walk, bo, &bo_needs_unlock))
			bo_locked = true;
		else if (!exec)
			continue;

		if (!ttm_bo_get_unless_zero(bo)) {
			xe_ttm_lru_walk_unlock(bo, bo_needs_unlock);
			continue;
		}

		spin_unlock(&bdev->lru_lock);

		if (!bo_locked) {
			ret = drm_exec_lock_obj(exec, &bo->base);
			if (ret)
				ttm_bo_put(bo);
		}

		lret = 0;

		/*
		 * Note that in between the release of the lru lock and the
		 * drm_exec_lock_obj, the bo may have switched resource,
		 * and also memory type. In that case we just skip it.
		 */
		if (bo->resource == res && res->mem_type == mem_type &&
		    walk->ops->allow_bo(walk, bo, mem_type))
			lret = walk->ops->process_bo(walk, bo);

		xe_ttm_lru_walk_unlock(bo, bo_needs_unlock);
		ttm_bo_put(bo);
		if (lret < 0) {
			sofar = lret;
			goto out;
		}

		sofar += lret;
		if (sofar >= target)
			goto out;

		spin_lock(&bdev->lru_lock);
	}
	spin_unlock(&bdev->lru_lock);
out:
	ttm_resource_cursor_fini(&cursor);
	return sofar;
}
