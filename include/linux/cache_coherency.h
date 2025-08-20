/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cache coherency maintenace operation device drivers
 *
 * Copyright Huawei 2025
 */
#ifndef _LINUX_CACHE_COHERENCY_H_
#define _LINUX_CACHE_COHERENCY_H_

#include <linux/list.h>
#include <linux/types.h>

struct cc_inval_params {
	phys_addr_t addr;
	size_t size;
};

struct cache_coherency_device;

struct coherency_ops {
	int (*wbinv)(struct cache_coherency_device *ccd, struct cc_inval_params *invp);
	int (*done)(struct cache_coherency_device *ccd);
};

struct cache_coherency_device {
	struct list_head node;
	const struct coherency_ops *ops;
};

int cache_coherency_device_register(struct cache_coherency_device *ccd);
void cache_coherency_device_unregister(struct cache_coherency_device *ccd);

struct cache_coherency_device *
_cache_coherency_device_alloc(const struct coherency_ops *ops, size_t size);
/**
 * cache_coherency_device_alloc - Allocate a cache coherency device
 * @ops: Cache maintenance operations
 * @drv_struct: structure that contains the struct cache_coherency_device
 * @member: Name of the struct cache_coherency_device member in @drv_struct.
 *
 * This allocates and initializes the cache_coherency_device embedded in the
 * drv_struct. Upon success the pointer must be freed via
 * cache_coherency_device_free().
 *
 * Returns a 'drv_struct \*' on success, NULL on error.
 */
#define cache_coherency_device_alloc(ops, drv_struct, member)	    \
	({								    \
		static_assert(__same_type(struct cache_coherency_device,    \
					  ((drv_struct *)NULL)->member));   \
		static_assert(offsetof(drv_struct, member) == 0);	    \
		(drv_struct *)_cache_coherency_device_alloc(ops,	    \
			sizeof(drv_struct));				    \
	})
void cache_coherency_device_free(struct cache_coherency_device *ccd);

#endif
