// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021-2022 Intel Corporation
 * Copyright (C) 2021-2022 Red Hat
 */

#include <drm/drm_managed.h>
#include <drm/drm_drv.h>
#include <drm/drm_buddy.h>

#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>

#include "xe_bo.h"
#include "xe_configfs.h"
#include "xe_device.h"
#include "xe_exec_queue.h"
#include "xe_lrc.h"
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
		down_write(&vm->lock);
		xe_vm_kill(vm, true);
		up_write(&vm->lock);
	}
	if (vm)
		xe_vm_put(vm);

	xe_bo_lock(bo, false);
	/*  Ban exec queue if BO is lrc */
	if (bo->q && xe_exec_queue_get_unless_zero(bo->q)) {
		/* ban queue */
		xe_exec_queue_kill(bo->q);
		xe_exec_queue_put(bo->q);
	}

	xe_bo_set_purgeable_state(bo, XE_MADV_PURGEABLE_DONTNEED);
	ttm_bo_unmap_virtual(&bo->ttm);   /* nuke CPU mmap + VRAM IO mappings */
	if (xe_bo_is_pinned(bo))
		xe_bo_unpin(bo);
	ret = xe_ttm_bo_purge(&bo->ttm, &ctx);
	xe_bo_unlock(bo);

	return ret;
}

static bool xe_ttm_vram_page_already_processed(struct xe_ttm_vram_mgr *mgr,
					       unsigned long addr)
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

static int xe_ttm_vram_reserve_page_at_addr(struct xe_device *xe, unsigned long addr,
					    struct xe_ttm_vram_mgr *vram_mgr, struct gpu_buddy *mm)
{
	struct xe_ttm_vram_offline_resource *nentry;
	struct ttm_buffer_object *tbo = NULL;
	struct gpu_buddy_block *block;
	enum reserve_status {
		pending = 0,
		fail
	};
	u64 size = SZ_4K;
	int ret = 0;

	scoped_guard(mutex, &vram_mgr->lock) {
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

			WARN_ON(!block->private);
			tbo = block->private;
			pbo = ttm_to_xe_bo(tbo);

			/* Get reference safely - BO may have zero refcount */
			if (!xe_bo_get_unless_zero(pbo)) {
				kfree(nentry);
				return -ENOENT;
			}
			/* Critical kernel BO? */
			if ((pbo->ttm.type == ttm_bo_type_kernel &&
			     !(pbo->flags & XE_BO_FLAG_PINNED_LATE_RESTORE)) ||
			    (xe_bo_is_user(pbo) && xe_bo_is_pinned(pbo))) {
				kfree(nentry);
				xe_ttm_vram_free_bad_pages(&xe->drm, vram_mgr);
				xe_bo_put(pbo);
				drm_err(&xe->drm,
					"%s: addr: 0x%lx is critical kernel bo, requesting SBR\n",
					__func__, addr);
				/* Hint System controller driver for reset with -EIO  */
				return -EIO;
			}
			nentry->id = ++vram_mgr->n_queued_pages;
			list_add(&nentry->queued_link, &vram_mgr->queued_pages);
		}
	}
	if (block) {
		struct xe_ttm_vram_offline_resource *pos, *n;
		struct xe_bo *pbo = ttm_to_xe_bo(tbo);

		/* Purge BO containing address - reference held from above */
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
				drm_warn(&xe->drm, "Could not reserve page at addr:0x%lx, ret:%d\n",
					 addr, ret);
				nentry->status = fail;
				return ret;
			}

			list_for_each_entry_safe(pos, n, &vram_mgr->queued_pages, queued_link) {
				if (pos->id == nentry->id) {
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
		scoped_guard(mutex, &vram_mgr->lock) {
			ret = xe_ttm_vram_buddy_alloc(vram_mgr, addr, addr + size,
						      size, size, &nentry->blocks,
						      GPU_BUDDY_RANGE_ALLOCATION,
						      NULL, &nentry->used_visible_size);
			if (ret) {
				drm_warn(&xe->drm, "Could not reserve page at addr:0x%lx, ret:%d\n",
					 addr, ret);
				nentry->status = fail;
				return ret;
			}

			nentry->id = ++vram_mgr->n_offlined_pages;
			list_add(&nentry->offlined_link, &vram_mgr->offlined_pages);
			/* RAS will send command to FW for offlining page based on ret value */
		}
	}
	/* Success */
	return ret;
}

static struct xe_vram_region *xe_ttm_vram_addr_to_region(struct xe_device *xe,
							 resource_size_t addr)
{
	unsigned long stolen_base = xe_ttm_stolen_gpu_offset(xe);
	struct xe_vram_region *vr;
	struct xe_tile *tile;
	int id;

	/* Addr from stolen memory? */
	if (addr + SZ_4K >= stolen_base)
		return NULL;

	for_each_tile(tile, xe, id) {
		vr = tile->mem.vram;
		if ((addr <= vr->dpa_base + vr->actual_physical_size) &&
		    (addr + SZ_4K >= vr->dpa_base))
			return vr;
	}
	return NULL;
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
int xe_ttm_vram_handle_addr_fault(struct xe_device *xe, unsigned long addr)
{
	struct xe_ttm_vram_mgr *vram_mgr;
	struct xe_vram_region *vr;
	struct gpu_buddy *mm;
        bool policy;

	vr = xe_ttm_vram_addr_to_region(xe, addr);
	if (!vr) {
		drm_err(&xe->drm, "%s:%d addr:%lx error requesting SBR\n",
			__func__, __LINE__, addr);
		/* Hint System controller driver for reset with -EIO  */
		return -EIO;
	}
	vram_mgr = &vr->ttm;
	mm = &vram_mgr->mm;

	scoped_guard(mutex, &vram_mgr->lock) {
		if (xe_ttm_vram_page_already_processed(vram_mgr, addr))
			return -EEXIST;
	}

	policy = xe_configfs_get_bad_page_reservation(to_pci_dev(xe->drm.dev));
	if (!policy) {
		drm_err(&xe->drm, "0x%lx is reported as corrupted address by HW\n",
			addr);
		/* Let RAS report to FW to drop addr from SRAM queue */
		return -EOPNOTSUPP;
	}

	/* Reserve page at address */
	return xe_ttm_vram_reserve_page_at_addr(xe, addr, vram_mgr, mm);
}
EXPORT_SYMBOL(xe_ttm_vram_handle_addr_fault);

static void xe_ttm_vram_dump_bad_pages_info(char *buf, struct xe_ttm_vram_mgr *mgr)
{
	const unsigned int element_size = sizeof("0xabcdabcd : 0x12345678 : R\n") - 1;
	const unsigned int maxpage_size = sizeof("max_pages: 10000\n") - 1;
	struct xe_ttm_vram_offline_resource *pos, *n;
	struct gpu_buddy_block *block;
	ssize_t s = 0;

	mutex_lock(&mgr->lock);
	s += scnprintf(&buf[s], maxpage_size + 1, "max_pages: %d\n", mgr->max_pages);
	list_for_each_entry_safe(pos, n, &mgr->offlined_pages, offlined_link) {
		block = list_first_entry(&pos->blocks,
					 struct gpu_buddy_block,
					 link);
		s += scnprintf(&buf[s], element_size + 1,
			       "0x%08llx : 0x%08llx : %1s\n",
			       gpu_buddy_block_offset(block) >> PAGE_SHIFT,
			       gpu_buddy_block_size(&mgr->mm, block),
			       "R");
	}
	list_for_each_entry_safe(pos, n, &mgr->queued_pages, queued_link) {
		block = list_first_entry(&pos->blocks,
					 struct gpu_buddy_block,
					 link);
		s += scnprintf(&buf[s], element_size + 1,
			       "0x%08llx : 0x%08llx : %1s\n",
			       gpu_buddy_block_offset(block) >> PAGE_SHIFT,
			       gpu_buddy_block_size(&mgr->mm, block),
			       pos->status ? "P" : "F");
	}
	mutex_unlock(&mgr->lock);
}

static ssize_t vram_bad_pages_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xe_device *xe = pdev_to_xe_device(pdev);
	struct ttm_resource_manager *man;
	struct xe_ttm_vram_mgr *mgr;

	man = ttm_manager_type(&xe->ttm, XE_PL_VRAM0);
	if (man) {
		mgr = to_xe_ttm_vram_mgr(man);
		xe_ttm_vram_dump_bad_pages_info(buf, mgr);
	}

	return sysfs_emit(buf, "%s\n", buf);
}
static DEVICE_ATTR_RO(vram_bad_pages);

static void xe_ttm_vram_sysfs_fini(void *arg)
{
	struct xe_device *xe = arg;

	device_remove_file(xe->drm.dev, &dev_attr_vram_bad_pages);
}

/**
 * xe_ttm_vram_sysfs_init - Initialize vram sysfs component
 * @tile: Xe Tile object
 *
 * It needs to be initialized after the main tile component is ready
 *
 * Returns: 0 on success, negative error code on error.
 */
int xe_ttm_vram_sysfs_init(struct xe_device *xe)
{
	int err;

	err = device_create_file(xe->drm.dev, &dev_attr_vram_bad_pages);
	if (err) {
		dev_err(xe->drm.dev, "Failed to create vram_bad_pages sysfs file: %d\n", err);
		return 0;
	}

	return devm_add_action_or_reset(xe->drm.dev, xe_ttm_vram_sysfs_fini, xe);
}
EXPORT_SYMBOL(xe_ttm_vram_sysfs_init);
