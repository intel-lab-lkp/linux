/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cache coherency device drivers
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

struct device;
struct cache_coherency_device {
	struct list_head node;
	struct device *parent;
	const struct coherency_ops *ops;
};

int cache_coherency_device_register(struct cache_coherency_device *ccd);
void cache_coherency_device_unregister(struct cache_coherency_device *ccd);

struct cache_coherency_device *
cache_coherency_alloc_device(struct device *parent,
			      const struct coherency_ops *ops, size_t size);
void cache_coherency_device_free(struct cache_coherency_device *ccd);

#endif
