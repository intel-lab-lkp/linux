// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <kunit/test.h>
#include <kunit/visibility.h>

#include <uapi/linux/sysinfo.h>

#include "tests/xe_bo_test.h"
#include "tests/xe_pci_test.h"
#include "tests/xe_test.h"

#include "xe_bo_evict.h"
#include "xe_pci.h"
#include "xe_pm.h"

static int ccs_test_migrate(struct xe_tile *tile, struct xe_bo *bo,
			    bool clear, u64 get_val, u64 assign_val,
			    struct kunit *test)
{
	struct dma_fence *fence;
	struct ttm_tt *ttm;
	struct page *page;
	pgoff_t ccs_page;
	long timeout;
	u64 *cpu_map;
	int ret;
	u32 offset;

	/* Move bo to VRAM if not already there. */
	ret = xe_bo_validate(bo, NULL, false);
	if (ret) {
		KUNIT_FAIL(test, "Failed to validate bo.\n");
		return ret;
	}

	/* Optionally clear bo *and* CCS data in VRAM. */
	if (clear) {
		fence = xe_migrate_clear(tile->migrate, bo, bo->ttm.resource);
		if (IS_ERR(fence)) {
			KUNIT_FAIL(test, "Failed to submit bo clear.\n");
			return PTR_ERR(fence);
		}
		dma_fence_put(fence);
	}

	/* Evict to system. CCS data should be copied. */
	ret = xe_bo_evict(bo, true);
	if (ret) {
		KUNIT_FAIL(test, "Failed to evict bo.\n");
		return ret;
	}

	/* Sync all migration blits */
	timeout = dma_resv_wait_timeout(bo->ttm.base.resv,
					DMA_RESV_USAGE_KERNEL,
					true,
					5 * HZ);
	if (timeout <= 0) {
		KUNIT_FAIL(test, "Failed to sync bo eviction.\n");
		return -ETIME;
	}

	/*
	 * Bo with CCS data is now in system memory. Verify backing store
	 * and data integrity. Then assign for the next testing round while
	 * we still have a CPU map.
	 */
	ttm = bo->ttm.ttm;
	if (!ttm || !ttm_tt_is_populated(ttm)) {
		KUNIT_FAIL(test, "Bo was not in expected placement.\n");
		return -EINVAL;
	}

	ccs_page = xe_bo_ccs_pages_start(bo) >> PAGE_SHIFT;
	if (ccs_page >= ttm->num_pages) {
		KUNIT_FAIL(test, "No TTM CCS pages present.\n");
		return -EINVAL;
	}

	page = ttm->pages[ccs_page];
	cpu_map = kmap_local_page(page);

	/* Check first CCS value */
	if (cpu_map[0] != get_val) {
		KUNIT_FAIL(test,
			   "Expected CCS readout 0x%016llx, got 0x%016llx.\n",
			   (unsigned long long)get_val,
			   (unsigned long long)cpu_map[0]);
		ret = -EINVAL;
	}

	/* Check last CCS value, or at least last value in page. */
	offset = xe_device_ccs_bytes(tile_to_xe(tile), bo->size);
	offset = min_t(u32, offset, PAGE_SIZE) / sizeof(u64) - 1;
	if (cpu_map[offset] != get_val) {
		KUNIT_FAIL(test,
			   "Expected CCS readout 0x%016llx, got 0x%016llx.\n",
			   (unsigned long long)get_val,
			   (unsigned long long)cpu_map[offset]);
		ret = -EINVAL;
	}

	cpu_map[0] = assign_val;
	cpu_map[offset] = assign_val;
	kunmap_local(cpu_map);

	return ret;
}

static void ccs_test_run_tile(struct xe_device *xe, struct xe_tile *tile,
			      struct kunit *test)
{
	struct xe_bo *bo;

	int ret;

	/* TODO: Sanity check */
	unsigned int bo_flags = XE_BO_FLAG_VRAM_IF_DGFX(tile);

	if (IS_DGFX(xe))
		kunit_info(test, "Testing vram id %u\n", tile->id);
	else
		kunit_info(test, "Testing system memory\n");

	bo = xe_bo_create_user(xe, NULL, NULL, SZ_1M, DRM_XE_GEM_CPU_CACHING_WC,
			       ttm_bo_type_device, bo_flags);
	if (IS_ERR(bo)) {
		KUNIT_FAIL(test, "Failed to create bo.\n");
		return;
	}

	xe_bo_lock(bo, false);

	kunit_info(test, "Verifying that CCS data is cleared on creation.\n");
	ret = ccs_test_migrate(tile, bo, false, 0ULL, 0xdeadbeefdeadbeefULL,
			       test);
	if (ret)
		goto out_unlock;

	kunit_info(test, "Verifying that CCS data survives migration.\n");
	ret = ccs_test_migrate(tile, bo, false, 0xdeadbeefdeadbeefULL,
			       0xdeadbeefdeadbeefULL, test);
	if (ret)
		goto out_unlock;

	kunit_info(test, "Verifying that CCS data can be properly cleared.\n");
	ret = ccs_test_migrate(tile, bo, true, 0ULL, 0ULL, test);

out_unlock:
	xe_bo_unlock(bo);
	xe_bo_put(bo);
}

static int ccs_test_run_device(struct xe_device *xe)
{
	struct kunit *test = xe_cur_kunit();
	struct xe_tile *tile;
	int id;

	if (!xe_device_has_flat_ccs(xe)) {
		kunit_info(test, "Skipping non-flat-ccs device.\n");
		return 0;
	}

	xe_pm_runtime_get(xe);

	for_each_tile(tile, xe, id) {
		/* For igfx run only for primary tile */
		if (!IS_DGFX(xe) && id > 0)
			continue;
		ccs_test_run_tile(xe, tile, test);
	}

	xe_pm_runtime_put(xe);

	return 0;
}

void xe_ccs_migrate_kunit(struct kunit *test)
{
	xe_call_for_each_device(ccs_test_run_device);
}
EXPORT_SYMBOL_IF_KUNIT(xe_ccs_migrate_kunit);

static int evict_test_run_tile(struct xe_device *xe, struct xe_tile *tile, struct kunit *test)
{
	struct xe_bo *bo, *external;
	unsigned int bo_flags = XE_BO_FLAG_VRAM_IF_DGFX(tile);
	struct xe_vm *vm = xe_migrate_get_vm(xe_device_get_root_tile(xe)->migrate);
	struct xe_gt *__gt;
	int err, i, id;

	kunit_info(test, "Testing device %s vram id %u\n",
		   dev_name(xe->drm.dev), tile->id);

	for (i = 0; i < 2; ++i) {
		xe_vm_lock(vm, false);
		bo = xe_bo_create_user(xe, NULL, vm, 0x10000,
				       DRM_XE_GEM_CPU_CACHING_WC,
				       ttm_bo_type_device,
				       bo_flags);
		xe_vm_unlock(vm);
		if (IS_ERR(bo)) {
			KUNIT_FAIL(test, "bo create err=%pe\n", bo);
			break;
		}

		external = xe_bo_create_user(xe, NULL, NULL, 0x10000,
					     DRM_XE_GEM_CPU_CACHING_WC,
					     ttm_bo_type_device, bo_flags);
		if (IS_ERR(external)) {
			KUNIT_FAIL(test, "external bo create err=%pe\n", external);
			goto cleanup_bo;
		}

		xe_bo_lock(external, false);
		err = xe_bo_pin_external(external);
		xe_bo_unlock(external);
		if (err) {
			KUNIT_FAIL(test, "external bo pin err=%pe\n",
				   ERR_PTR(err));
			goto cleanup_external;
		}

		err = xe_bo_evict_all(xe);
		if (err) {
			KUNIT_FAIL(test, "evict err=%pe\n", ERR_PTR(err));
			goto cleanup_all;
		}

		for_each_gt(__gt, xe, id)
			xe_gt_sanitize(__gt);
		err = xe_bo_restore_kernel(xe);
		/*
		 * Snapshotting the CTB and copying back a potentially old
		 * version seems risky, depending on what might have been
		 * inflight. Also it seems snapshotting the ADS object and
		 * copying back results in serious breakage. Normally when
		 * calling xe_bo_restore_kernel() we always fully restart the
		 * GT, which re-intializes such things.  We could potentially
		 * skip saving and restoring such objects in xe_bo_evict_all()
		 * however seems quite fragile not to also restart the GT. Try
		 * to do that here by triggering a GT reset.
		 */
		for_each_gt(__gt, xe, id) {
			xe_gt_reset_async(__gt);
			flush_work(&__gt->reset.worker);
		}
		if (err) {
			KUNIT_FAIL(test, "restore kernel err=%pe\n",
				   ERR_PTR(err));
			goto cleanup_all;
		}

		err = xe_bo_restore_user(xe);
		if (err) {
			KUNIT_FAIL(test, "restore user err=%pe\n", ERR_PTR(err));
			goto cleanup_all;
		}

		if (!xe_bo_is_vram(external)) {
			KUNIT_FAIL(test, "external bo is not vram\n");
			err = -EPROTO;
			goto cleanup_all;
		}

		if (xe_bo_is_vram(bo)) {
			KUNIT_FAIL(test, "bo is vram\n");
			err = -EPROTO;
			goto cleanup_all;
		}

		if (i) {
			down_read(&vm->lock);
			xe_vm_lock(vm, false);
			err = xe_bo_validate(bo, bo->vm, false);
			xe_vm_unlock(vm);
			up_read(&vm->lock);
			if (err) {
				KUNIT_FAIL(test, "bo valid err=%pe\n",
					   ERR_PTR(err));
				goto cleanup_all;
			}
			xe_bo_lock(external, false);
			err = xe_bo_validate(external, NULL, false);
			xe_bo_unlock(external);
			if (err) {
				KUNIT_FAIL(test, "external bo valid err=%pe\n",
					   ERR_PTR(err));
				goto cleanup_all;
			}
		}

		xe_bo_lock(external, false);
		xe_bo_unpin_external(external);
		xe_bo_unlock(external);

		xe_bo_put(external);

		xe_bo_lock(bo, false);
		__xe_bo_unset_bulk_move(bo);
		xe_bo_unlock(bo);
		xe_bo_put(bo);
		continue;

cleanup_all:
		xe_bo_lock(external, false);
		xe_bo_unpin_external(external);
		xe_bo_unlock(external);
cleanup_external:
		xe_bo_put(external);
cleanup_bo:
		xe_bo_lock(bo, false);
		__xe_bo_unset_bulk_move(bo);
		xe_bo_unlock(bo);
		xe_bo_put(bo);
		break;
	}

	xe_vm_put(vm);

	return 0;
}

static int evict_test_run_device(struct xe_device *xe)
{
	struct kunit *test = xe_cur_kunit();
	struct xe_tile *tile;
	int id;

	if (!IS_DGFX(xe)) {
		kunit_info(test, "Skipping non-discrete device %s.\n",
			   dev_name(xe->drm.dev));
		return 0;
	}

	xe_pm_runtime_get(xe);

	for_each_tile(tile, xe, id)
		evict_test_run_tile(xe, tile, test);

	xe_pm_runtime_put(xe);

	return 0;
}

void xe_bo_evict_kunit(struct kunit *test)
{
	xe_call_for_each_device(evict_test_run_device);
}
EXPORT_SYMBOL_IF_KUNIT(xe_bo_evict_kunit);

struct xe_bo_link {
	struct list_head link;
	struct xe_bo *bo;
};

#define XE_BO_SHRINK_SIZE ((unsigned long)SZ_64M)

/*
 * Try to create system bos corresponding to twice the amount
 * of available system memory to test shrinker functionality.
 * If no swap space is available to accommodate the
 * memory overcommit, mark bos purgeable.
 */
static int shrink_test_run_device(struct xe_device *xe)
{
	struct kunit *test = xe_cur_kunit();
	LIST_HEAD(bos);
	struct xe_bo_link *link, *next;
	struct sysinfo si;
	size_t total, alloced;
	unsigned int interrupted = 0, successful = 0;

	si_meminfo(&si);
	total = si.freeram * si.mem_unit;

	kunit_info(test, "Free ram is %lu bytes. Will allocate twice of that.\n",
		   total);

	total <<= 1;
	for (alloced = 0; alloced < total ; alloced += XE_BO_SHRINK_SIZE) {
		struct xe_bo *bo;
		unsigned int mem_type;

		link = kzalloc(sizeof(*link), GFP_KERNEL);
		if (!link) {
			KUNIT_FAIL(test, "Unexpeced link allocation failure\n");
			break;
		}

		INIT_LIST_HEAD(&link->link);

		/* We can create bos using WC caching here. But it is slower. */
		bo = xe_bo_create_user(xe, NULL, NULL, XE_BO_SHRINK_SIZE,
				       DRM_XE_GEM_CPU_CACHING_WB,
				       ttm_bo_type_device,
				       XE_BO_FLAG_SYSTEM);
		if (IS_ERR(bo)) {
			if (bo != ERR_PTR(-ENOMEM) && bo != ERR_PTR(-ENOSPC) &&
			    bo != ERR_PTR(-EINTR) && bo != ERR_PTR(-ERESTARTSYS))
				KUNIT_FAIL(test, "Error creating bo: %pe\n", bo);
			kfree(link);
			break;
		}
		link->bo = bo;
		list_add_tail(&link->link, &bos);
		xe_bo_lock(bo, false);

		/*
		 * If we're low on swap entries, we can't shrink unless the bo
		 * is marked purgeable.
		 */
		if (get_nr_swap_pages() < (XE_BO_SHRINK_SIZE >> PAGE_SHIFT) * 128) {
			struct xe_ttm_tt *xe_tt =
				container_of(bo->ttm.ttm, typeof(*xe_tt), ttm);
			long num_pages = xe_tt->ttm.num_pages;

			xe_tt->purgeable = true;
			xe_shrinker_mod_pages(xe->mem.shrinker, -num_pages,
					      num_pages);
		}

		mem_type = bo->ttm.resource->mem_type;
		xe_bo_unlock(bo);
		if (mem_type != XE_PL_TT)
			KUNIT_FAIL(test, "Bo in incorrect memory type: %u\n",
				   bo->ttm.resource->mem_type);
		cond_resched();
		if (signal_pending(current))
			break;
	}

	/* Read back and destroy bos */
	list_for_each_entry_safe_reverse(link, next, &bos, link) {
		static struct ttm_operation_ctx ctx = {.interruptible = true};
		struct xe_bo *bo = link->bo;
		int ret;

		if (!signal_pending(current)) {
			xe_bo_lock(bo, NULL);
			ret = ttm_bo_validate(&bo->ttm, &tt_placement, &ctx);
			xe_bo_unlock(bo);
			if (ret && ret != -EINTR)
				KUNIT_FAIL(test, "Validation failed: %pe\n",
					   ERR_PTR(ret));
			else if (ret)
				interrupted++;
			else
				successful++;
		}
		xe_bo_put(link->bo);
		list_del(&link->link);
		kfree(link);
		cond_resched();
	}
	kunit_info(test, "Readbacks interrupted: %u successful: %u\n",
		   interrupted, successful);

	return 0;
}

void xe_bo_shrink_kunit(struct kunit *test)
{
	xe_call_for_each_device(shrink_test_run_device);
}
EXPORT_SYMBOL_IF_KUNIT(xe_bo_shrink_kunit);
