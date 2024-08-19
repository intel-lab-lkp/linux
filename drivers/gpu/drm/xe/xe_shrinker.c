// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/shrinker.h>
#include <linux/swap.h>

#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_tt.h>

#include "xe_bo.h"
#include "xe_pm.h"
#include "xe_shrinker.h"

/**
 * struct xe_shrinker - per-device shrinker
 * @xe: Back pointer to the device.
 * @lock: Lock protecting accounting.
 * @shrinkable_pages: Number of pages that are currently shrinkable.
 * @purgeable_pages: Number of pages that are currently purgeable.
 * @shrink: Pointer to the mm shrinker.
 * @pm_worker: Worker to wake up the device if required.
 */
struct xe_shrinker {
	struct xe_device *xe;
	rwlock_t lock;
	long shrinkable_pages;
	long purgeable_pages;
	struct shrinker *shrink;
	struct work_struct pm_worker;
};

/**
 * struct xe_shrink_lru_walk - lru_walk subclass for shrinker
 * @walk: The embedded base class.
 * @xe: Pointer to the xe device.
 * @purge: Purgeable only request from the srinker.
 * @writeback: Try to write back to persistent storage.
 */
struct xe_shrink_lru_walk {
	struct ttm_lru_walk walk;
	struct xe_device *xe;
	bool purge;
	bool writeback;
};

static struct xe_shrinker *to_xe_shrinker(struct shrinker *shrink)
{
	return shrink->private_data;
}

static struct xe_shrink_lru_walk *
to_xe_shrink_lru_walk(struct ttm_lru_walk *walk)
{
	return container_of(walk, struct xe_shrink_lru_walk, walk);
}

/**
 * xe_shrinker_mod_pages() - Modify shrinker page accounting
 * @shrinker: Pointer to the struct xe_shrinker.
 * @shrinkable: Shrinkable pages delta. May be negative.
 * @purgeable: Purgeable page delta. May be negative.
 *
 * Modifies the shrinkable and purgeable pages accounting.
 */
void
xe_shrinker_mod_pages(struct xe_shrinker *shrinker, long shrinkable, long purgeable)
{
	write_lock(&shrinker->lock);
	shrinker->shrinkable_pages += shrinkable;
	shrinker->purgeable_pages += purgeable;
	write_unlock(&shrinker->lock);
}

static s64 xe_shrinker_process_bo(struct ttm_lru_walk *walk, struct ttm_buffer_object *bo)
{
	struct xe_shrink_lru_walk *shrink_walk = to_xe_shrink_lru_walk(walk);

	return xe_bo_shrink(walk, bo, (struct xe_bo_shrink_flags)
			    {.purge = shrink_walk->purge,
			     .writeback = shrink_walk->writeback});
}

static s64 xe_shrinker_walk(struct xe_shrink_lru_walk *shrink_walk, s64 target)
{
	struct xe_device *xe = shrink_walk->xe;
	struct ttm_resource_manager *man;
	unsigned int mem_type;
	s64 progress = 0;
	s64 lret;

	for (mem_type = XE_PL_SYSTEM; mem_type <= XE_PL_TT; ++mem_type) {
		man = ttm_manager_type(&xe->ttm, mem_type);
		if (!man || !man->use_tt)
			continue;

		lret = ttm_lru_walk_for_evict(&shrink_walk->walk, &xe->ttm, man, target);
		if (lret < 0)
			return lret;

		progress += lret;
		if (progress >= target)
			break;
	}

	return progress;
}

static unsigned long
xe_shrinker_count(struct shrinker *shrink, struct shrink_control *sc)
{
	struct xe_shrinker *shrinker = to_xe_shrinker(shrink);
	unsigned long num_pages;

	num_pages = get_nr_swap_pages();
	read_lock(&shrinker->lock);
	num_pages = min_t(unsigned long, num_pages, shrinker->shrinkable_pages);
	num_pages += shrinker->purgeable_pages;
	read_unlock(&shrinker->lock);

	return num_pages ? num_pages : SHRINK_EMPTY;
}

static const struct ttm_lru_walk_ops xe_shrink_ops = {
	.process_bo = xe_shrinker_process_bo,
};

/*
 * Check if we need runtime pm, and if so try to grab a reference if
 * already active. If grabbing a reference fails, queue a worker that
 * does it for us outside of reclaim, but don't wait for it to complete.
 * If bo shrinking needs an rpm reference and we don't have it (yet),
 * that bo will be skipped anyway.
 */
static bool xe_shrinker_runtime_pm_get(struct xe_shrinker *shrinker, bool force,
				       unsigned long nr_to_scan)
{
	struct xe_device *xe = shrinker->xe;

	if (IS_DGFX(xe) || !xe_device_has_flat_ccs(xe) ||
	    !get_nr_swap_pages())
		return false;

	if (!force) {
		read_lock(&shrinker->lock);
		force = (nr_to_scan > shrinker->purgeable_pages);
		read_unlock(&shrinker->lock);
		if (!force)
			return false;
	}

	if (!xe_pm_runtime_get_if_active(xe)) {
		queue_work(xe->unordered_wq, &shrinker->pm_worker);
		return false;
	}

	return true;
}

static void xe_shrinker_runtime_pm_put(struct xe_shrinker *shrinker, bool runtime_pm)
{
	if (runtime_pm)
		xe_pm_runtime_put(shrinker->xe);
}

static unsigned long xe_shrinker_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	struct xe_shrinker *shrinker = to_xe_shrinker(shrink);
	bool is_kswapd = current_is_kswapd();
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = !is_kswapd,
	};
	unsigned long nr_to_scan, freed = 0;
	struct xe_shrink_lru_walk shrink_walk = {
		.walk = {
			.ops = &xe_shrink_ops,
			.ctx = &ctx,
			.trylock_only = true,
		},
		.xe = shrinker->xe,
		.purge = true,
		.writeback = is_kswapd,
	};
	bool runtime_pm;
	bool purgeable;
	s64 ret;

	sc->nr_scanned = 0;
	nr_to_scan = sc->nr_to_scan;

	read_lock(&shrinker->lock);
	purgeable = !!shrinker->purgeable_pages;
	read_unlock(&shrinker->lock);

	/* Might need runtime PM. Try to wake early if it looks like it. */
	runtime_pm = xe_shrinker_runtime_pm_get(shrinker, false, nr_to_scan);

	while (purgeable && freed < nr_to_scan) {
		ret = xe_shrinker_walk(&shrink_walk, nr_to_scan);
		if (ret <= 0)
			break;

		freed += ret;
	}

	sc->nr_scanned = freed;
	if (freed < nr_to_scan)
		nr_to_scan -= freed;
	else
		nr_to_scan = 0;
	if (!nr_to_scan)
		goto out;

	/* If we didn't wake before, try to do it now if needed. */
	if (!runtime_pm)
		runtime_pm = xe_shrinker_runtime_pm_get(shrinker, true, 0);

	shrink_walk.purge = false;
	nr_to_scan = sc->nr_to_scan;
	while (freed < nr_to_scan) {
		ret = xe_shrinker_walk(&shrink_walk, nr_to_scan);
		if (ret <= 0)
			break;

		freed += ret;
	}

	sc->nr_scanned = freed;

out:
	xe_shrinker_runtime_pm_put(shrinker, runtime_pm);
	return freed ? freed : SHRINK_STOP;
}

/* Wake up the device for shrinking. */
static void xe_shrinker_pm(struct work_struct *work)
{
	struct xe_shrinker *shrinker =
		container_of(work, typeof(*shrinker), pm_worker);

	xe_pm_runtime_get(shrinker->xe);
	xe_pm_runtime_put(shrinker->xe);
}

/**
 * xe_shrinker_create() - Create an xe per-device shrinker
 * @xe: Pointer to the xe device.
 *
 * Returns: A pointer to the created shrinker on success,
 * Negative error code on failure.
 */
struct xe_shrinker *xe_shrinker_create(struct xe_device *xe)
{
	struct xe_shrinker *shrinker = kzalloc(sizeof(*shrinker), GFP_KERNEL);

	if (!shrinker)
		return ERR_PTR(-ENOMEM);

	shrinker->shrink = shrinker_alloc(0, "xe system shrinker");
	if (!shrinker->shrink) {
		kfree(shrinker);
		return ERR_PTR(-ENOMEM);
	}

	INIT_WORK(&shrinker->pm_worker, xe_shrinker_pm);
	shrinker->xe = xe;
	rwlock_init(&shrinker->lock);
	shrinker->shrink->count_objects = xe_shrinker_count;
	shrinker->shrink->scan_objects = xe_shrinker_scan;
	shrinker->shrink->private_data = shrinker;
	shrinker_register(shrinker->shrink);

	return shrinker;
}

/**
 * xe_shrinker_destroy() - Destroy an xe per-device shrinker
 * @shrinker: Pointer to the shrinker to destroy.
 */
void xe_shrinker_destroy(struct xe_shrinker *shrinker)
{
	xe_assert(shrinker->xe, !shrinker->shrinkable_pages);
	xe_assert(shrinker->xe, !shrinker->purgeable_pages);
	shrinker_free(shrinker->shrink);
	flush_work(&shrinker->pm_worker);
	kfree(shrinker);
}
