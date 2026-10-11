// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 NVIDIA Corporation & Affiliates */
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/memregion.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/slab.h>

#include <cxl/hdm.h>
#include "cxl.h"
#include "core.h"

/*
 * CXL state that must stay available while no CXL driver is bound: the CXL
 * locks, the per-device HDM cache filled during PCI enumeration, and the
 * region quiesce used around a reset.
 */
struct cxl_rwsem cxl_rwsem = {
	.region = __RWSEM_INITIALIZER(cxl_rwsem.region),
	.dpa = __RWSEM_INITIALIZER(cxl_rwsem.dpa),
};
EXPORT_SYMBOL_FOR_MODULES(cxl_rwsem, "cxl_core");

/* A published cache is usable only if its HDM register block was mapped. */
bool cxl_hdm_cache_present(struct cxl_hdm_info *const *slot)
{
	guard(rwsem_read)(&cxl_rwsem.dpa);
	return *slot && (*slot)->hdm_size;
}

/**
 * cxl_hdm_cache_publish() - publish HDM state read during enumeration
 * @slot: cache pointer owned by the device, for example &pci_dev->hdm
 * @info: state to publish
 *
 * Ownership of @info passes to @slot, or @info is freed if another caller
 * published first.
 */
void cxl_hdm_cache_publish(struct cxl_hdm_info **slot,
			   struct cxl_hdm_info *info)
{
	struct cxl_hdm_info *new __free(kfree) = info;

	guard(rwsem_write)(&cxl_rwsem.dpa);
	/* Another initializer may have published while we read MMIO. */
	if (!*slot)
		*slot = no_free_ptr(new);
}

/**
 * cxl_hdm_cache_release() - unpublish and free cached HDM state
 * @slot: cache pointer owned by the device
 *
 * Take the DPA write lock to exclude readers while clearing @slot, then
 * free the unpublished cache after releasing the lock.
 */
void cxl_hdm_cache_release(struct cxl_hdm_info **slot)
{
	struct cxl_hdm_info *info;

	scoped_guard(rwsem_write, &cxl_rwsem.dpa) {
		info = *slot;
		/* Unpublish before freeing so readers cannot see stale state. */
		*slot = NULL;
	}
	kfree(info);
}

/**
 * cxl_hdm_cache_dvsec_decode() - check for memory decoded by DVSEC ranges
 * @slot: cache pointer owned by the device
 *
 * Return: true if the published state shows CXL.mem enabled while HDM
 * decoders are globally disabled, so that the device decodes memory through
 * its DVSEC ranges, which the cache does not describe.
 */
bool cxl_hdm_cache_dvsec_decode(struct cxl_hdm_info *const *slot)
{
	struct cxl_hdm_info *info;

	guard(rwsem_read)(&cxl_rwsem.dpa);
	info = *slot;
	return info && (info->dvsec_ctrl & PCI_DVSEC_CXL_MEM_ENABLE) &&
	       !(info->global_ctrl & CXL_HDM_DECODER_ENABLE);
}

/**
 * cxl_hdm_cache_snapshot() - copy the published HDM state
 * @slot: cache pointer owned by the device
 *
 * Return: a copy the caller frees with kfree(), ERR_PTR(-ENXIO) when nothing
 * is published, or another ERR_PTR() on failure.
 */
struct cxl_hdm_info *cxl_hdm_cache_snapshot(struct cxl_hdm_info *const *slot)
{
	struct cxl_hdm_info *info, *copy;
	size_t size;

	guard(rwsem_read)(&cxl_rwsem.dpa);
	info = *slot;
	if (!info)
		return ERR_PTR(-ENXIO);
	if (info->decoder_count <= 0)
		return ERR_PTR(-EINVAL);

	size = struct_size(info, settings, info->decoder_count);
	if (size == SIZE_MAX)
		return ERR_PTR(-EOVERFLOW);

	copy = kmemdup(info, size, GFP_KERNEL);
	if (!copy)
		return ERR_PTR(-ENOMEM);

	return copy;
}

/**
 * cxl_hdm_cache_copy_decoders() - refresh the decoders of a snapshot
 * @slot: cache pointer owned by the device
 * @dst: snapshot of the same device from cxl_hdm_cache_snapshot()
 *
 * Copy only the per-decoder settings, which region commit and teardown keep
 * current. Leave the rest of @dst, including global and DVSEC control, as
 * captured: a refresh during a reset may have read those registers from a
 * device that returned all-ones.
 *
 * Return: 0 on success, -ENXIO when nothing is published, or -EINVAL when
 * @dst holds a different number of decoders.
 */
int cxl_hdm_cache_copy_decoders(struct cxl_hdm_info *const *slot,
				struct cxl_hdm_info *dst)
{
	struct cxl_hdm_info *info;

	lockdep_assert_held_write(&cxl_rwsem.region);
	guard(rwsem_read)(&cxl_rwsem.dpa);
	info = *slot;
	if (!info)
		return -ENXIO;
	if (info->decoder_count != dst->decoder_count)
		return -EINVAL;

	memcpy(dst->settings, info->settings,
	       flex_array_size(info, settings, info->decoder_count));
	return 0;
}

void cxl_region_quiesce_lock(void)
	__acquires(cxl_region_quiesce)
	__context_unsafe(token maps to cxl_rwsem.region)
{
	down_write(&cxl_rwsem.region);
	__acquire(cxl_region_quiesce);
}

void cxl_region_quiesce_unlock(void)
	__releases(cxl_region_quiesce)
	__context_unsafe(token maps to cxl_rwsem.region)
{
	__release(cxl_region_quiesce);
	up_write(&cxl_rwsem.region);
}

/*
 * Bound the range twice: request_mem_region() takes resource_size_t while
 * cpu_cache_invalidate_memregion() takes size_t, and the two differ on
 * 32-bit builds with CONFIG_PHYS_ADDR_T_64BIT. range_len() can also reach
 * RESOURCE_SIZE_MAX + 1 for a full-width range, and wraps to zero when
 * resource_size_t is 64-bit, which the !len test catches.
 */
static int cxl_hdm_range_validate(struct device *dev,
				  const struct range *hpa_range)
{
	u64 len = range_len(hpa_range);

	if (!len)
		return -EINVAL;

	if (hpa_range->end > RESOURCE_SIZE_MAX) {
		dev_err(dev,
			"CXL reset range [%#llx-%#llx] exceeds resource address size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	if (len > RESOURCE_SIZE_MAX) {
		dev_err(dev,
			"CXL reset range [%#llx-%#llx] exceeds resource size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	if (len > SIZE_MAX) {
		dev_err(dev,
			"CXL reset range [%#llx-%#llx] exceeds cache flush size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	return 0;
}

static int cxl_hdm_range_add(struct cxl_hdm_quiesce *q, struct device *dev,
			     const struct range *hpa_range)
{
	struct cxl_hdm_range *range, *next, *new_range;
	int rc;

	rc = cxl_hdm_range_validate(dev, hpa_range);
	if (rc)
		return rc;

	list_for_each_entry(range, &q->ranges, list)
		if (range_contains(&range->hpa_range, hpa_range))
			return 0;

	new_range = kzalloc_obj(*new_range);
	if (!new_range)
		return -ENOMEM;

	new_range->dev = dev;
	new_range->hpa_range = *hpa_range;
	new_range->len = range_len(hpa_range);

	list_for_each_entry_safe(range, next, &q->ranges, list) {
		if (range_contains(hpa_range, &range->hpa_range)) {
			list_del(&range->list);
			kfree(range);
		}
	}
	list_add_tail(&new_range->list, &q->ranges);

	return 0;
}

static int cxl_hdm_ranges_collect(struct cxl_hdm_quiesce *q, struct device *dev,
				  const struct cxl_hdm_info *snapshot)
{
	int rc;

	for (int i = 0; i < snapshot->decoder_count; i++) {
		const struct cxl_decoder_config *config =
			&snapshot->settings[i].config;

		/* A committed zero-size decoder maps no HPA. */
		if (!(config->flags & CXL_DECODER_F_ENABLE) ||
		    !range_len(&config->hpa_range))
			continue;

		if (config->flags & CXL_DECODER_F_NORMALIZED_ADDRESSING) {
			dev_err(dev,
				"CXL reset does not support normalized address decoders\n");
			return -EOPNOTSUPP;
		}

		rc = cxl_hdm_range_add(q, dev, &config->hpa_range);
		if (rc)
			return rc;
	}

	return 0;
}

static int cxl_hdm_ranges_request(struct cxl_hdm_quiesce *q)
{
	struct cxl_hdm_range *range;

	lockdep_assert_held_write(&cxl_rwsem.region);

	list_for_each_entry(range, &q->ranges, list) {
		const struct range *hpa_range = &range->hpa_range;

		range->res = request_mem_region(hpa_range->start, range->len,
						"cxl_reset");
		if (!range->res) {
			dev_err(range->dev,
				"cannot reset while CXL memory range is busy [%#llx-%#llx]\n",
				hpa_range->start, hpa_range->end);
			return -EBUSY;
		}
	}

	return 0;
}

static int cxl_hdm_ranges_invalidate(struct cxl_hdm_quiesce *q)
{
	struct cxl_hdm_range *range;
	int rc = 0;

	lockdep_assert_held_write(&cxl_rwsem.region);

	list_for_each_entry(range, &q->ranges, list) {
		const struct range *hpa_range = &range->hpa_range;
		int rc2;

		rc2 = cpu_cache_invalidate_memregion(hpa_range->start, range->len);
		if (rc2)
			dev_err(range->dev,
				"failed to invalidate CPU cache [%#llx-%#llx]: %d\n",
				hpa_range->start, hpa_range->end, rc2);
		rc = rc ?: rc2;
	}

	return rc;
}

/**
 * cxl_hdm_quiesce_prepare() - take the HPA ranges of a device out of use
 * @dev: device for error messages
 * @q: quiesce state to initialize
 * @snapshot: HDM state from cxl_hdm_cache_snapshot()
 *
 * Reserve every enabled system-physical range in @snapshot exclusively, then
 * write back and invalidate CPU caches for them. Call
 * cxl_hdm_quiesce_destroy() afterwards whether or not this succeeds.
 *
 * Return: 0 on success, or a negative errno.
 */
int cxl_hdm_quiesce_prepare(struct device *dev, struct cxl_hdm_quiesce *q,
			    const struct cxl_hdm_info *snapshot)
{
	int rc;

	lockdep_assert_held_write(&cxl_rwsem.region);
	INIT_LIST_HEAD(&q->ranges);

	if (!cpu_cache_has_invalidate_memregion()) {
		dev_err(dev, "CPU cache invalidation unavailable\n");
		return -ENXIO;
	}

	rc = cxl_hdm_ranges_collect(q, dev, snapshot);
	if (rc)
		return rc;

	rc = cxl_hdm_ranges_request(q);
	if (rc)
		return rc;

	return cxl_hdm_ranges_invalidate(q);
}

/* Evict lines fetched during reset before DMA exclusion ends. */
int cxl_hdm_quiesce_flush(struct cxl_hdm_quiesce *q)
{
	return cxl_hdm_ranges_invalidate(q);
}

void cxl_hdm_quiesce_destroy(struct cxl_hdm_quiesce *q)
{
	struct cxl_hdm_range *range, *next;

	list_for_each_entry_safe(range, next, &q->ranges, list) {
		list_del(&range->list);
		if (range->res)
			release_mem_region(range->hpa_range.start,
					   resource_size(range->res));
		kfree(range);
	}
}
