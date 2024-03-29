// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/shrinker.h>
#include <linux/swap.h>

#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_tt.h>

#include "xe_bo.h"
#include "xe_shrinker.h"
#include "xe_ttm_helpers.h"

/**
 * struct xe_shrinker - per-device shrinker
 * @xe: Back pointer to the device.
 * @lock: Lock protecting accounting.
 * @shrinkable_pages: Number of pages that are currently shrinkable.
 * @purgeable_pages: Number of pages that are currently purgeable.
 * @shrink: Pointer to the mm shrinker.
 */
struct xe_shrinker {
	struct xe_device *xe;
	rwlock_t lock;
	long shrinkable_pages;
	long purgeable_pages;
	struct shrinker *shrink;
};

/**
 * struct xe_shrink_lru_walk - lru_walk subclass for shrinker
 * @walk: The embedded base class.
 * @xe: Pointer to the xe device.
 * @purge: Purgeable only request from the srinker.
 */
struct xe_shrink_lru_walk {
	struct xe_ttm_lru_walk walk;
	struct xe_device *xe;
	bool purge;
};

static struct xe_shrinker *to_xe_shrinker(struct shrinker *shrink)
{
	return shrink->private_data;
}

static struct xe_shrink_lru_walk *
to_xe_shrink_lru_walk(struct xe_ttm_lru_walk *walk)
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

static bool xe_shrinker_allow_bo(struct xe_ttm_lru_walk *walk,
				 struct ttm_buffer_object *bo,
				 unsigned int mem_type)
{
	struct xe_shrink_lru_walk *shrink_walk = to_xe_shrink_lru_walk(walk);
	struct ttm_place place = {.mem_type = mem_type};
	struct ttm_tt *tt = bo->ttm;
	bool ret;

	if (!tt || !ttm_tt_is_populated(tt) ||
	    !(tt->page_flags & TTM_TT_FLAG_EXTERNAL_MAPPABLE))
		return false;

	if (shrink_walk->purge)
		return false;

	ret = ttm_bo_eviction_valuable(bo, &place);

	return ret;
}

static long xe_shrinker_walk(struct xe_shrink_lru_walk *shrink_walk, long target)
{
	struct xe_device *xe = shrink_walk->xe;
	struct ttm_resource_manager *man;
	unsigned int mem_type;
	long sofar = 0;
	long lret;

	for (mem_type = XE_PL_SYSTEM; mem_type <= XE_PL_TT; ++mem_type) {
		man = ttm_manager_type(&xe->ttm, mem_type);
		if (!man || !man->use_tt)
			continue;

		lret = xe_ttm_lru_walk_for_evict(&shrink_walk->walk,
						 &xe->ttm, man,
						 mem_type, target);
		if (lret < 0)
			return lret;

		sofar += lret;
		if (sofar >= target)
			break;
	}

	return sofar;
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

static const struct xe_ttm_lru_walk_ops xe_shrink_ops = {
	.process_bo = xe_bo_shrinker_process,
	.allow_bo = xe_shrinker_allow_bo,
};

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
		},
		.xe = shrinker->xe,
		.purge = true,
	};
	bool purgeable;
	long ret;

	sc->nr_scanned = 0;
	nr_to_scan = sc->nr_to_scan;

	read_lock(&shrinker->lock);
	purgeable = !!shrinker->purgeable_pages;
	read_unlock(&shrinker->lock);

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
		return freed ? freed : SHRINK_STOP;

	shrink_walk.purge = false;
	while (freed < nr_to_scan) {
		ret = xe_shrinker_walk(&shrink_walk, nr_to_scan);
		if (ret <= 0)
			break;

		freed += ret;
	}

	sc->nr_scanned = freed;

	return freed ? freed : SHRINK_STOP;
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
	kfree(shrinker);
}
