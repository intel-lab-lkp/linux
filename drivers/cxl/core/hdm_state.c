// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 NVIDIA Corporation & Affiliates */
#include <linux/cleanup.h>
#include <linux/export.h>
#include <linux/slab.h>

#include <cxl/hdm.h>
#include "cxl.h"
#include "core.h"

/*
 * CXL state that must stay available while no CXL driver is bound: the CXL
 * locks and the per-device HDM cache filled during PCI enumeration.
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
