// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021-2022 Intel Corporation
 * Copyright (C) 2021-2022 Red Hat
 */

#include <drm/drm_managed.h>
#include <drm/drm_drv.h>
#include <drm/drm_buddy.h>
#include <uapi/drm/xe_drm.h>

#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>

#include "regs/xe_regs.h"
#include "xe_bo.h"
#include "xe_configfs.h"
#include "xe_device.h"
#include "xe_exec_queue.h"
#include "xe_lrc.h"
#include "xe_mmio.h"
#include "xe_res_cursor.h"
#include "xe_ttm_stolen_mgr.h"
#include "xe_ttm_vram_mgr.h"
#include "xe_vram_types.h"

static inline struct gpu_buddy_block *
xe_ttm_vram_mgr_first_block(struct list_head *list)
{
	return list_first_entry_or_null(list, struct gpu_buddy_block, link);
}

static inline bool xe_is_vram_mgr_blocks_contiguous(struct gpu_buddy *mm,
						    struct list_head *head)
{
	struct gpu_buddy_block *block;
	u64 start, size;

	block = xe_ttm_vram_mgr_first_block(head);
	if (!block)
		return false;

	while (head != block->link.next) {
		start = gpu_buddy_block_offset(block);
		size = gpu_buddy_block_size(mm, block);

		block = list_entry(block->link.next, struct gpu_buddy_block,
				   link);
		if (start + size != gpu_buddy_block_offset(block))
			return false;
	}

	return true;
}

static int xe_ttm_vram_buddy_alloc(struct xe_ttm_vram_mgr *mgr, u64 start,
				   u64 end, u64 size, u64 min_page_size,
				   struct list_head *blocks, unsigned long flags,
				   void *priv, u64 *used_visible)
{
	struct gpu_buddy *mm = &mgr->mm;
	struct gpu_buddy_block *block;
	int err;

	err = gpu_buddy_alloc_blocks(mm, start, end, size, min_page_size, blocks, flags);
	if (err)
		return err;

	list_for_each_entry(block, blocks, link)
		block->private = priv;

	if (end <= mgr->visible_size) {
		*used_visible = size;
	} else {
		list_for_each_entry(block, blocks, link) {
			u64 blk_start = gpu_buddy_block_offset(block);

			if (blk_start < mgr->visible_size) {
				u64 blk_end = blk_start + gpu_buddy_block_size(mm, block);

				*used_visible += min(blk_end, mgr->visible_size) - blk_start;
			}
		}
	}

	mgr->visible_avail -= *used_visible;
	return 0;
}

static int xe_ttm_vram_mgr_new(struct ttm_resource_manager *man,
			       struct ttm_buffer_object *tbo,
			       const struct ttm_place *place,
			       struct ttm_resource **res)
{
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);
	struct xe_ttm_vram_mgr_resource *vres;
	struct gpu_buddy *mm = &mgr->mm;
	u64 size, min_page_size;
	unsigned long lpfn;
	int err;

	lpfn = place->lpfn;
	if (!lpfn || lpfn > man->size >> PAGE_SHIFT)
		lpfn = man->size >> PAGE_SHIFT;

	if (tbo->base.size >> PAGE_SHIFT > (lpfn - place->fpfn))
		return -E2BIG; /* don't trigger eviction for the impossible */

	vres = kzalloc_obj(*vres);
	if (!vres)
		return -ENOMEM;

	ttm_resource_init(tbo, place, &vres->base);

	/* bail out quickly if there's likely not enough VRAM for this BO */
	if (ttm_resource_manager_usage(man) > man->size) {
		err = -ENOSPC;
		goto error_fini;
	}

	INIT_LIST_HEAD(&vres->blocks);

	if (place->flags & TTM_PL_FLAG_TOPDOWN)
		vres->flags |= GPU_BUDDY_TOPDOWN_ALLOCATION;

	if (place->flags & TTM_PL_FLAG_CONTIGUOUS)
		vres->flags |= GPU_BUDDY_CONTIGUOUS_ALLOCATION;

	if (place->fpfn || lpfn != man->size >> PAGE_SHIFT)
		vres->flags |= GPU_BUDDY_RANGE_ALLOCATION;

	if (WARN_ON(!vres->base.size)) {
		err = -EINVAL;
		goto error_fini;
	}
	size = vres->base.size;

	min_page_size = mgr->default_page_size;
	if (tbo->page_alignment)
		min_page_size = (u64)tbo->page_alignment << PAGE_SHIFT;

	if (WARN_ON(min_page_size < mm->chunk_size)) {
		err = -EINVAL;
		goto error_fini;
	}

	if (WARN_ON(!IS_ALIGNED(size, min_page_size))) {
		err = -EINVAL;
		goto error_fini;
	}

	mutex_lock(&mgr->lock);
	if (lpfn <= mgr->visible_size >> PAGE_SHIFT && size > mgr->visible_avail) {
		err = -ENOSPC;
		goto error_unlock;
	}

	err = xe_ttm_vram_buddy_alloc(mgr, (u64)place->fpfn << PAGE_SHIFT,
				      (u64)lpfn << PAGE_SHIFT, size,
				      min_page_size, &vres->blocks, vres->flags,
				      tbo, &vres->used_visible_size);
	if (err)
		goto error_unlock;
	mutex_unlock(&mgr->lock);

	if (!(vres->base.placement & TTM_PL_FLAG_CONTIGUOUS) &&
	    xe_is_vram_mgr_blocks_contiguous(mm, &vres->blocks))
		vres->base.placement |= TTM_PL_FLAG_CONTIGUOUS;

	/*
	 * For some kernel objects we still rely on the start when io mapping
	 * the object.
	 */
	if (vres->base.placement & TTM_PL_FLAG_CONTIGUOUS) {
		struct gpu_buddy_block *block = list_first_entry(&vres->blocks,
								 typeof(*block),
								 link);

		vres->base.start = gpu_buddy_block_offset(block) >> PAGE_SHIFT;
	} else {
		vres->base.start = XE_BO_INVALID_OFFSET;
	}

	*res = &vres->base;
	return 0;
error_unlock:
	mutex_unlock(&mgr->lock);
error_fini:
	ttm_resource_fini(man, &vres->base);
	kfree(vres);

	return err;
}

static void xe_ttm_vram_buddy_free(struct xe_ttm_vram_mgr *mgr,
				   struct list_head *blocks,
				   u64 used_visible)
{
	struct gpu_buddy_block *block;

	list_for_each_entry(block, blocks, link)
		block->private = NULL;
	gpu_buddy_free_list(&mgr->mm, blocks, 0);
	mgr->visible_avail += used_visible;
}

static void xe_ttm_vram_mgr_del(struct ttm_resource_manager *man,
				struct ttm_resource *res)
{
	struct xe_ttm_vram_mgr_resource *vres =
		to_xe_ttm_vram_mgr_resource(res);
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);

	mutex_lock(&mgr->lock);
	xe_ttm_vram_buddy_free(mgr, &vres->blocks, vres->used_visible_size);
	mutex_unlock(&mgr->lock);

	ttm_resource_fini(man, res);

	kfree(vres);
}

static void xe_ttm_vram_mgr_debug(struct ttm_resource_manager *man,
				  struct drm_printer *printer)
{
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);
	struct gpu_buddy *mm = &mgr->mm;

	mutex_lock(&mgr->lock);
	drm_printf(printer, "default_page_size: %lluKiB\n",
		   mgr->default_page_size >> 10);
	drm_printf(printer, "visible_avail: %lluMiB\n",
		   (u64)mgr->visible_avail >> 20);
	drm_printf(printer, "visible_size: %lluMiB\n",
		   (u64)mgr->visible_size >> 20);

	drm_buddy_print(mm, printer);
	mutex_unlock(&mgr->lock);
	drm_printf(printer, "man size:%llu\n", man->size);
}

static bool xe_ttm_vram_mgr_intersects(struct ttm_resource_manager *man,
				       struct ttm_resource *res,
				       const struct ttm_place *place,
				       size_t size)
{
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);
	struct xe_ttm_vram_mgr_resource *vres =
		to_xe_ttm_vram_mgr_resource(res);
	struct gpu_buddy *mm = &mgr->mm;
	struct gpu_buddy_block *block;

	if (!place->fpfn && !place->lpfn)
		return true;

	if (!place->fpfn && place->lpfn == mgr->visible_size >> PAGE_SHIFT)
		return vres->used_visible_size > 0;

	list_for_each_entry(block, &vres->blocks, link) {
		unsigned long fpfn =
			gpu_buddy_block_offset(block) >> PAGE_SHIFT;
		unsigned long lpfn = fpfn +
			(gpu_buddy_block_size(mm, block) >> PAGE_SHIFT);

		if (place->fpfn < lpfn && place->lpfn > fpfn)
			return true;
	}

	return false;
}

static bool xe_ttm_vram_mgr_compatible(struct ttm_resource_manager *man,
				       struct ttm_resource *res,
				       const struct ttm_place *place,
				       size_t size)
{
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);
	struct xe_ttm_vram_mgr_resource *vres =
		to_xe_ttm_vram_mgr_resource(res);
	struct gpu_buddy *mm = &mgr->mm;
	struct gpu_buddy_block *block;

	if (!place->fpfn && !place->lpfn)
		return true;

	if (!place->fpfn && place->lpfn == mgr->visible_size >> PAGE_SHIFT)
		return vres->used_visible_size == size;

	list_for_each_entry(block, &vres->blocks, link) {
		unsigned long fpfn =
			gpu_buddy_block_offset(block) >> PAGE_SHIFT;
		unsigned long lpfn = fpfn +
			(gpu_buddy_block_size(mm, block) >> PAGE_SHIFT);

		if (fpfn < place->fpfn || lpfn > place->lpfn)
			return false;
	}

	return true;
}

static const struct ttm_resource_manager_func xe_ttm_vram_mgr_func = {
	.alloc	= xe_ttm_vram_mgr_new,
	.free	= xe_ttm_vram_mgr_del,
	.intersects = xe_ttm_vram_mgr_intersects,
	.compatible = xe_ttm_vram_mgr_compatible,
	.debug	= xe_ttm_vram_mgr_debug
};

static void xe_ttm_vram_free_bad_pages(struct drm_device *dev, struct xe_ttm_vram_mgr *mgr)
{
	struct xe_ttm_vram_offline_resource *pos, *n;

	list_for_each_entry_safe(pos, n, &mgr->offlined_pages, offlined_link) {
		xe_ttm_vram_buddy_free(mgr, &pos->blocks, pos->used_visible_size);
		list_del(&pos->offlined_link);
		--mgr->n_offlined_pages;
		kfree(pos);
	}
	list_for_each_entry_safe(pos, n, &mgr->queued_pages, queued_link) {
		xe_ttm_vram_buddy_free(mgr, &pos->blocks, 0);
		list_del(&pos->queued_link);
		--mgr->n_queued_pages;
		kfree(pos);
	}
}

static void xe_ttm_vram_mgr_fini(struct drm_device *dev, void *arg)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_ttm_vram_mgr *mgr = arg;
	struct ttm_resource_manager *man = &mgr->manager;

	ttm_resource_manager_set_used(man, false);

	if (ttm_resource_manager_evict_all(&xe->ttm, man))
		return;

	mutex_lock(&mgr->lock);
	xe_ttm_vram_free_bad_pages(dev, mgr);
	mutex_unlock(&mgr->lock);

	WARN_ON_ONCE(mgr->visible_avail != mgr->visible_size);

	gpu_buddy_fini(&mgr->mm);

	ttm_resource_manager_cleanup(&mgr->manager);

	ttm_set_driver_manager(&xe->ttm, mgr->mem_type, NULL);
}

int __xe_ttm_vram_mgr_init(struct xe_device *xe, struct xe_ttm_vram_mgr *mgr,
			   u32 mem_type, u64 size, u64 io_size,
			   u64 default_page_size)
{
	struct ttm_resource_manager *man = &mgr->manager;
	const char *name;
	int err;

	name = mem_type == XE_PL_VRAM0 ? "vram0" : "vram1";
	man->cg = drmm_cgroup_register_region(&xe->drm, name, size);
	if (IS_ERR(man->cg))
		return PTR_ERR(man->cg);

	man->func = &xe_ttm_vram_mgr_func;
	mgr->mem_type = mem_type;
	err = drmm_mutex_init(&xe->drm, &mgr->lock);
	if (err)
		return err;
	INIT_LIST_HEAD(&mgr->offlined_pages);
	INIT_LIST_HEAD(&mgr->queued_pages);
	mgr->default_page_size = default_page_size;
	mgr->visible_size = io_size;
	mgr->visible_avail = io_size;

	ttm_resource_manager_init(man, &xe->ttm, size);
	err = gpu_buddy_init(&mgr->mm, man->size, default_page_size);
	if (err)
		return err;

	gpu_buddy_driver_set_lock(&mgr->mm, &mgr->lock);
	ttm_set_driver_manager(&xe->ttm, mem_type, &mgr->manager);
	ttm_resource_manager_set_used(&mgr->manager, true);

	return drmm_add_action_or_reset(&xe->drm, xe_ttm_vram_mgr_fini, mgr);
}

/**
 * xe_ttm_vram_mgr_init - initialize TTM VRAM region
 * @xe: pointer to Xe device
 * @vram: pointer to xe_vram_region that contains the memory region attributes
 *
 * Initialize the Xe TTM for given @vram region using the given parameters.
 *
 * Returns 0 for success, negative error code otherwise.
 */
int xe_ttm_vram_mgr_init(struct xe_device *xe, struct xe_vram_region *vram)
{
	return __xe_ttm_vram_mgr_init(xe, &vram->ttm, vram->placement,
				      xe_vram_region_usable_size(vram),
				      xe_vram_region_io_size(vram),
				      PAGE_SIZE);
}

int xe_ttm_vram_mgr_alloc_sgt(struct xe_device *xe,
			      struct ttm_resource *res,
			      u64 offset, u64 length,
			      struct device *dev,
			      enum dma_data_direction dir,
			      struct sg_table **sgt)
{
	struct xe_tile *tile = &xe->tiles[res->mem_type - XE_PL_VRAM0];
	struct xe_ttm_vram_mgr_resource *vres = to_xe_ttm_vram_mgr_resource(res);
	struct xe_res_cursor cursor;
	struct scatterlist *sg;
	int num_entries = 0;
	int i, r;

	if (vres->used_visible_size < res->size)
		return -EOPNOTSUPP;

	*sgt = kmalloc_obj(**sgt);
	if (!*sgt)
		return -ENOMEM;

	/* Determine the number of GPU_BUDDY blocks to export */
	xe_res_first(res, offset, length, &cursor);
	while (cursor.remaining) {
		num_entries++;
		/* Limit maximum size to 2GiB due to SG table limitations. */
		xe_res_next(&cursor, min_t(u64, cursor.size, SZ_2G));
	}

	r = sg_alloc_table(*sgt, num_entries, GFP_KERNEL);
	if (r)
		goto error_free;

	/* Initialize scatterlist nodes of sg_table */
	for_each_sgtable_sg((*sgt), sg, i)
		sg->length = 0;

	/*
	 * Walk down GPU_BUDDY blocks to populate scatterlist nodes
	 * @note: Use iterator api to get first the GPU_BUDDY block
	 * and the number of bytes from it. Access the following
	 * GPU_BUDDY block(s) if more buffer needs to exported
	 */
	xe_res_first(res, offset, length, &cursor);
	for_each_sgtable_sg((*sgt), sg, i) {
		phys_addr_t phys = cursor.start + xe_vram_region_io_start(tile->mem.vram);
		size_t size = min_t(u64, cursor.size, SZ_2G);
		dma_addr_t addr;

		addr = dma_map_resource(dev, phys, size, dir,
					DMA_ATTR_SKIP_CPU_SYNC);
		r = dma_mapping_error(dev, addr);
		if (r)
			goto error_unmap;

		sg_set_page(sg, NULL, size, 0);
		sg_dma_address(sg) = addr;
		sg_dma_len(sg) = size;

		xe_res_next(&cursor, size);
	}

	return 0;

error_unmap:
	for_each_sgtable_sg((*sgt), sg, i) {
		if (!sg->length)
			continue;

		dma_unmap_resource(dev, sg->dma_address,
				   sg->length, dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(*sgt);

error_free:
	kfree(*sgt);
	return r;
}

void xe_ttm_vram_mgr_free_sgt(struct device *dev, enum dma_data_direction dir,
			      struct sg_table *sgt)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_sg(sgt, sg, i)
		dma_unmap_resource(dev, sg->dma_address,
				   sg->length, dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

u64 xe_ttm_vram_get_cpu_visible_size(struct ttm_resource_manager *man)
{
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);

	return mgr->visible_size;
}

void xe_ttm_vram_get_used(struct ttm_resource_manager *man,
			  u64 *used, u64 *used_visible)
{
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);

	mutex_lock(&mgr->lock);
	*used = mgr->mm.size - mgr->mm.avail;
	*used_visible = mgr->visible_size - mgr->visible_avail;
	mutex_unlock(&mgr->lock);
}

u64 xe_ttm_vram_get_avail(struct ttm_resource_manager *man)
{
	struct xe_ttm_vram_mgr *mgr = to_xe_ttm_vram_mgr(man);
	u64 avail;

	mutex_lock(&mgr->lock);
	avail =  mgr->mm.avail;
	mutex_unlock(&mgr->lock);

	return avail;
}

static int xe_ttm_vram_purge_page(struct xe_device *xe, struct xe_bo *bo)
{
	struct ttm_operation_ctx ctx = {};
	struct xe_exec_queue *q_to_put = NULL;
	struct xe_exec_queue *q = NULL;
	struct xe_vm *vm = NULL;
	u32	flags;
	int ret = 0;

	xe_bo_lock(bo, false);
	if (bo->vm)
		vm = xe_vm_get(bo->vm);
	flags = bo->flags;
	xe_bo_unlock(bo);
	/*  Ban VM if BO is PPGTT */
	if (vm && (flags & XE_BO_FLAG_PAGETABLE)) {
		struct xe_exec_queue *eq;

		down_write(&vm->lock);
		list_for_each_entry(eq, &vm->preempt.exec_queues, lr.link)
			atomic_or(DRM_XE_EXEC_QUEUE_BAN_REASON_PAGE_OFFLINE, &eq->ban_reason);
		smp_wmb(); /* Force all queue bits to be visible before killing the VM */
		xe_vm_kill(vm, true);
		up_write(&vm->lock);
	}
	if (vm)
		xe_vm_put(vm);

	xe_bo_lock(bo, false);
	q = READ_ONCE(bo->q);
	/*  Ban exec queue if BO is lrc */
	if (q && xe_exec_queue_get_unless_zero(q)) {
		/* ban queue */
                atomic_or(DRM_XE_EXEC_QUEUE_BAN_REASON_PAGE_OFFLINE, &q->ban_reason);
                smp_wmb(); /* Force bit change to finish before state change triggers */
                q_to_put = q;
	}

	if (bo->purgeable.state == XE_MADV_PURGEABLE_PURGED) {
		/* Already purged by shrinker during unlocked window — nothing to do */
		xe_bo_unlock(bo);
		goto out;
	}

	xe_bo_set_purgeable_state(bo, XE_MADV_PURGEABLE_DONTNEED);
	ttm_bo_unmap_virtual(&bo->ttm);   /* nuke CPU mmap + VRAM IO mappings */
	if (xe_bo_is_pinned(bo))
		xe_bo_unpin(bo);
	ret = xe_ttm_bo_purge(&bo->ttm, &ctx);
	xe_bo_unlock(bo);

out:
	if (q_to_put) {
		xe_exec_queue_kill(q_to_put);
		xe_exec_queue_put(q_to_put);
	}

	return ret;
}

static bool xe_ttm_vram_page_already_processed(struct xe_ttm_vram_mgr *mgr,
					       u64 addr)
{
	struct xe_ttm_vram_offline_resource *pos;

	lockdep_assert_held(&mgr->lock);

	list_for_each_entry(pos, &mgr->offlined_pages, offlined_link) {
		if (pos->addr == addr)
			return true;
	}

	list_for_each_entry(pos, &mgr->queued_pages, queued_link) {
		if (pos->addr == addr)
			return true;
	}

	return false;
}

static int xe_ttm_vram_reserve_page_at_addr(struct xe_device *xe, u64 addr,
					    struct xe_ttm_vram_mgr *vram_mgr, struct gpu_buddy *mm)
{
	struct xe_ttm_vram_offline_resource *nentry;
	struct ttm_buffer_object *tbo = NULL;
	struct xe_bo *pbo_to_put = NULL;
	struct gpu_buddy_block *block;
	enum reserve_status {
		pending = 0,
		fail
	};
	u64 size = SZ_4K;
	int ret = 0;

	scoped_guard(mutex, &vram_mgr->lock) {
		if (xe_ttm_vram_page_already_processed(vram_mgr, addr))
			return -EEXIST;
		block = gpu_buddy_allocated_addr_to_block(mm, addr);
		if (WARN_ON(IS_ERR(block)))
			return PTR_ERR(block);

		nentry = kzalloc_obj(*nentry);
		if (!nentry)
			return -ENOMEM;
		INIT_LIST_HEAD(&nentry->blocks);
		nentry->status = pending;
		nentry->addr = addr;

		if (block) {
			struct xe_bo *pbo;

			if (!block->private) {
				/* Race: another thread just reserved this block */
				kfree(nentry);
				return -EEXIST;
			}
			tbo = block->private;
			pbo = ttm_to_xe_bo(tbo);

			/* Get reference safely - BO may have zero refcount */
			if (!xe_bo_get_unless_zero(pbo)) {
				kfree(nentry);
				return -ENOENT;
			}
			/*
			 * Critical kernel BO? Best-effort check without resv lock;
			 * worst case a concurrent pin causes reset path unnecessarily.
			 */
			if ((pbo->ttm.type == ttm_bo_type_kernel &&
			     !(pbo->flags & XE_BO_FLAG_PINNED_LATE_RESTORE)) ||
			    (xe_bo_is_user(pbo) && xe_bo_is_pinned(pbo))) {
				kfree(nentry);
				pbo_to_put = pbo;
				drm_err(&xe->drm,
					"%s: addr: 0x%llx is critical kernel bo, requesting SBR\n",
					__func__, addr);
				break;
			}
			++vram_mgr->n_queued_pages;
			list_add(&nentry->queued_link, &vram_mgr->queued_pages);
		}
	}

	/* Deferred put outside lock to avoid recursive deadlock */
	if (pbo_to_put) {
		xe_bo_put(pbo_to_put);
		/* Hint System controller driver for reset with -EIO  */
		return -EIO;
	}

	if (block) {
		struct xe_ttm_vram_offline_resource *pos, *n;
		struct xe_bo *pbo = ttm_to_xe_bo(tbo);

		/*
		 * Purge BO containing address - reference held from above.
		 * Note: brief window between purge (freeing blocks) and re-reserve
		 * below. If another allocation claims the block, buddy_alloc fails
		 * and the next HW fault at this address will retry.
		 */
		ret = xe_ttm_vram_purge_page(xe, pbo);
		xe_bo_put(pbo);
		if (ret) {
			nentry->status = fail;
			return ret;
		}

		/* Reserve page at address addr*/
		scoped_guard(mutex, &vram_mgr->lock) {
			ret = xe_ttm_vram_buddy_alloc(vram_mgr, addr, addr + size,
						      size, size, &nentry->blocks,
						      GPU_BUDDY_RANGE_ALLOCATION,
						      NULL, &nentry->used_visible_size);
			if (ret) {
				drm_warn(&xe->drm,
					 "Could not reserve page at addr:0x%llx, ret:%d\n",
					 addr, ret);
				nentry->status = fail;
				return ret;
			}

			list_for_each_entry_safe(pos, n, &vram_mgr->queued_pages, queued_link) {
				if (pos->addr == nentry->addr) {
					--vram_mgr->n_queued_pages;
					list_del(&pos->queued_link);
					break;
				}
			}
			list_add(&nentry->offlined_link, &vram_mgr->offlined_pages);
			/* RAS will send command to FW for offlining page based on ret value */
			++vram_mgr->n_offlined_pages;
			return ret;
		}
	} else {
		struct xe_ttm_vram_offline_resource *pos, *n;

		scoped_guard(mutex, &vram_mgr->lock) {
			list_add(&nentry->queued_link, &vram_mgr->queued_pages);
			ret = xe_ttm_vram_buddy_alloc(vram_mgr, addr, addr + size,
						      size, size, &nentry->blocks,
						      GPU_BUDDY_RANGE_ALLOCATION,
						      NULL, &nentry->used_visible_size);
			if (ret) {
				drm_warn(&xe->drm,
					 "Could not reserve page at addr:0x%llx, ret:%d\n",
					 addr, ret);
				nentry->status = fail;
				return ret;
			}

			list_for_each_entry_safe(pos, n, &vram_mgr->queued_pages, queued_link) {
				if (pos->addr == nentry->addr) {
					--vram_mgr->n_queued_pages;
					list_del(&pos->queued_link);
					break;
				}
			}
			++vram_mgr->n_offlined_pages;
			list_add(&nentry->offlined_link, &vram_mgr->offlined_pages);
			/* RAS will send command to FW for offlining page based on ret value */
		}
	}
	/* Success */
	return ret;
}

static struct xe_vram_region *xe_ttm_vram_addr_to_region(struct xe_device *xe, u64 addr)
{
	u64 raw_offset = xe_mmio_read64_2x32(&xe_device_get_root_tile(xe)->mmio, GSMBASE);
	/* force a 4K (4096 bytes) page alignment */
	u64 gsmbase_dpa = raw_offset & ~(u64)(PAGE_SIZE - 1);
	struct xe_vram_region *vr;
	struct xe_tile *tile;
	int id;

	/* Addr from GSM? */
	if (addr >= gsmbase_dpa)
		/* Return NULL so the caller can request reset (SBR) */
		return NULL;

	for_each_tile(tile, xe, id) {
		vr = tile->mem.vram;
		if (addr >= vr->dpa_base &&
		    addr < vr->dpa_base + vr->usable_size)
			return vr;
	}

	/*
	 * Return an explicit error pointer so the caller knows the addr
	 * is invalid and should be ignored, NOT SBR.
	 */
	return ERR_PTR(-ENOENT);
}

/**
 * xe_ttm_vram_handle_addr_fault - Handle vram physical address error flaged
 * @xe: pointer to parent device
 * @addr: physical faulty address
 *
 * Handle the physcial faulty address error on specific tile.
 *
 * Returns 0 for success, negative error code otherwise as follow:
 * * %-EIO - critical BO or address outside any VRAM region; next action is reset.
 * * %-EOPNOTSUPP - log-only policy; no further action.
 * * %-ENOMEM - allocation failure; next action is reset.
 * * %-ENXIO - address not found in buddy; next action is reset.
 * * %-EEXIST - address already processed; no further action.
 * * % Any other negative error - next action is reset.
 */
int xe_ttm_vram_handle_addr_fault(struct xe_device *xe, u64 addr)
{
	struct xe_ttm_vram_mgr *vram_mgr;
	struct xe_vram_region *vr;
	struct gpu_buddy *mm;
	bool policy;

	vr = xe_ttm_vram_addr_to_region(xe, addr);
	if (IS_ERR(vr)) {
		/*
		 * The addr is outside VRAM and GSM.
		 * Log a debug message if needed, and safely exit/ignore.
		 */
		drm_dbg(&xe->drm, "Address %llx is out of bounds, ignoring fault.\n", addr);
		return -EOPNOTSUPP;
	}
	if (!vr) {
		drm_err(&xe->drm, "%s:%d GSM addr:%llx error requesting SBR\n",
			__func__, __LINE__, addr);
		/* Hint System controller driver for reset with -EIO  */
		return -EIO;
	}
	vram_mgr = &vr->ttm;
	mm = &vram_mgr->mm;

	policy = xe_configfs_get_bad_page_reservation(to_pci_dev(xe->drm.dev));
	if (!policy) {
		drm_err(&xe->drm, "0x%llx is reported as corrupted address by HW\n",
			addr);
		/* Let RAS report to FW to drop addr from SRAM queue */
		return -EOPNOTSUPP;
	}

	/* Reserve page at address */
	return xe_ttm_vram_reserve_page_at_addr(xe, addr, vram_mgr, mm);
}
EXPORT_SYMBOL(xe_ttm_vram_handle_addr_fault);

/**
 * xe_ttm_vram_inject_fault - Inject a VRAM page fault for testing
 * @xe: xe device instance
 *
 * Picks the last unallocated VRAM page and reports it as faulted
 * via xe_ttm_vram_handle_addr_fault(). Used by the fault-inject
 * debugfs interface for testing page offlining.
 *
 * Return: 0 on success, negative error code on failure.
 */
int xe_ttm_vram_inject_fault(struct xe_device *xe)
{
	struct xe_tile *tile = xe_device_get_root_tile(xe);
	struct xe_vram_region *vr = tile->mem.vram;
	struct xe_ttm_vram_mgr *vram_mgr = &vr->ttm;
	struct gpu_buddy *mm = &vram_mgr->mm;
	u64 addr;

	if (vr->actual_physical_size < SZ_4K)
		return -ENOSPC;

	addr = vr->actual_physical_size - SZ_4K;
	while (addr < vr->actual_physical_size) {
		struct gpu_buddy_block *block;
		bool found = false;

		scoped_guard(mutex, &vram_mgr->lock) {
			block = gpu_buddy_allocated_addr_to_block(mm, addr);
			if (!block)
				found = true;
		}

		/*
		 * Intentional race window: xe_ttm_vram_handle_addr_fault()
		 * re-acquires vram_mgr->lock internally, so we cannot hold
		 * it here. A concurrent allocation claiming this page between
		 * the two calls is an acceptable false negative for this
		 * test-only path.
		 */
		if (found)
			return xe_ttm_vram_handle_addr_fault(xe, addr + vr->dpa_base);

		cond_resched();
		if (addr == 0)
			break;
		addr -= SZ_4K;
	}

	return -ENOSPC;
}
EXPORT_SYMBOL(xe_ttm_vram_inject_fault);

static size_t serialize_bad_pages(struct xe_ttm_vram_mgr *mgr, char *buf, size_t max_len)
{
	struct xe_ttm_vram_offline_resource *pos;
	struct gpu_buddy_block *block;
	size_t s = 0;
	int printed;
	int count = 0;

	lockdep_assert_held(&mgr->lock);

	printed = scnprintf(buf + s, max_len - s, "max_pages: %d\n", mgr->max_pages);
	s += printed;

	list_for_each_entry(pos, &mgr->offlined_pages, offlined_link) {
		if (count >= 10000 || s >= max_len)
			break;

		block = list_first_entry_or_null(&pos->blocks, struct gpu_buddy_block, link);
		if (!block)
			continue;

		printed = scnprintf(buf + s, max_len - s, "0x%016llx : 0x%016llx : %c\n",
				    gpu_buddy_block_offset(block) >> PAGE_SHIFT,
				    gpu_buddy_block_size(&mgr->mm, block), 'R');
		s += printed;
		count++;
	}
	list_for_each_entry(pos, &mgr->queued_pages, queued_link) {
		u64 pfn, blk_size;

		if (count >= 10000 || s >= max_len)
			break;

		block = list_first_entry_or_null(&pos->blocks, struct gpu_buddy_block, link);
		if (block) {
			pfn = gpu_buddy_block_offset(block) >> PAGE_SHIFT;
			blk_size = gpu_buddy_block_size(&mgr->mm, block);
		} else {
			pfn = pos->addr >> PAGE_SHIFT;
			blk_size = PAGE_SIZE;
		}

		printed = scnprintf(buf + s, max_len - s, "0x%016llx : 0x%016llx : %c\n",
				    pfn, blk_size, pos->status ? 'F' : 'P');
		s += printed;
		count++;
	}

	return s;
}

static ssize_t vram_bad_pages_bin_read(struct file *filp, struct kobject *kobj,
				       const struct bin_attribute *attr, char *buf,
				       loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ttm_resource_manager *man;
	struct xe_ttm_vram_mgr *mgr;
	size_t allocation_size;
	struct xe_device *xe;
	size_t full_data_len;
	int active_entries;
	char *temp_buf;

	xe = pdev_to_xe_device(pdev);
	man = ttm_manager_type(&xe->ttm, XE_PL_VRAM0);
	if (!man)
		return -ENODEV;
	mgr = to_xe_ttm_vram_mgr(man);

	/* Snapshot entry count under lock, then allocate outside to avoid deadlock */
	mutex_lock(&mgr->lock);
	active_entries = mgr->n_offlined_pages + mgr->n_queued_pages;
	mutex_unlock(&mgr->lock);

	if (active_entries > 10000)
		active_entries = 10000;

	allocation_size = 64 + (active_entries * 48);

	temp_buf = kvmalloc(allocation_size, GFP_KERNEL);
	if (!temp_buf)
		return -ENOMEM;

	mutex_lock(&mgr->lock);
	full_data_len = serialize_bad_pages(mgr, temp_buf, allocation_size);
	mutex_unlock(&mgr->lock);

	if (off >= full_data_len) {
		kvfree(temp_buf);
		return 0;
	}

	if (off + count > full_data_len)
		count = full_data_len - off;

	memcpy(buf, temp_buf + off, count);

	kvfree(temp_buf);
	return count;
}

static const struct bin_attribute bin_attr_vram_bad_pages = {
	.attr = { .name = "vram_bad_pages", .mode = 0444 },
	.read = vram_bad_pages_bin_read,
	.size = 0,
};

static void xe_ttm_vram_sysfs_fini(void *arg)
{
	struct xe_device *xe = arg;
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);

	sysfs_remove_bin_file(&pdev->dev.kobj, &bin_attr_vram_bad_pages);
}

/**
 * xe_ttm_vram_sysfs_init - Initialize vram bad pages sysfs binary file
 * @xe: Xe Device object
 *
 * Creates a binary sysfs file under the PCI device for reading
 * offlined and queued VRAM pages. Supports large entry counts
 * via offset/count pagination.
 *
 * Returns: 0 on success, negative error code on error.
 */
int xe_ttm_vram_sysfs_init(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	int err;

	err = sysfs_create_bin_file(&pdev->dev.kobj, &bin_attr_vram_bad_pages);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to create vram_bad_pages sysfs: %d\n",
			err);
		return err;
	}

	return devm_add_action_or_reset(&pdev->dev, xe_ttm_vram_sysfs_fini, xe);
}
EXPORT_SYMBOL(xe_ttm_vram_sysfs_init);
