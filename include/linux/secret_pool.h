/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SECRET_POOL_H
#define _LINUX_SECRET_POOL_H

#include <linux/gfp.h>
#include <linux/numa.h>
#include <linux/slab.h>
#include <linux/types.h>

void *secret_pool_alloc_node(size_t size, gfp_t flags, int node)
	__alloc_size(1);

static inline __alloc_size(1) void *secret_pool_alloc(size_t size, gfp_t flags)
{
	return secret_pool_alloc_node(size, flags, NUMA_NO_NODE);
}

static inline __alloc_size(1) void *secret_pool_zalloc_node(size_t size,
							    gfp_t flags,
							    int node)
{
	return secret_pool_alloc_node(size, flags | __GFP_ZERO, node);
}

static inline __alloc_size(1) void *secret_pool_zalloc(size_t size,
						       gfp_t flags)
{
	return secret_pool_alloc_node(size, flags | __GFP_ZERO, NUMA_NO_NODE);
}

static inline void secret_pool_free(const void *objp)
{
	kfree_sensitive(objp);
}

#define secret_pool_alloc_obj(P, ...) \
	__alloc_objs(secret_pool_alloc, default_gfp(__VA_ARGS__), typeof(P), 1)
#define secret_pool_zalloc_obj(P, ...) \
	__alloc_objs(secret_pool_zalloc, default_gfp(__VA_ARGS__), typeof(P), 1)
#define secret_pool_alloc_flex(P, FAM, COUNT, ...) \
	__alloc_flex(secret_pool_alloc, default_gfp(__VA_ARGS__), typeof(P), \
		     FAM, COUNT)
#define secret_pool_zalloc_flex(P, FAM, COUNT, ...) \
	__alloc_flex(secret_pool_zalloc, default_gfp(__VA_ARGS__), typeof(P), \
		     FAM, COUNT)

#endif /* _LINUX_SECRET_POOL_H */
