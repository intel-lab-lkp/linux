// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Oracle and/or its affiliates.
 *
 * Pools for Send and Receive buffers.
 *
 * A buffer pool attempts to conserve both the number of DMA mappings
 * and the device's IOVA space by collecting small buffers together
 * into a chunk that has a single DMA mapping.
 *
 * Future work:
 *   - Manage pool resources by reference count
 *   - Manage chunk free space via a bitmap
 */

#include <linux/list.h>
#include <linux/sunrpc/svc_rdma.h>

#include <rdma/ib_verbs.h>

#include "pool.h"

struct rpcrdma_pool {
	struct list_head	rp_chunk_list;

	struct ib_device	*rp_device;
	size_t			rp_chunksize;
	size_t			rp_bufsize;
	enum dma_data_direction	rp_direction;
};

struct rpcrdma_pool_chunk {
	struct list_head	pc_next_chunk;

	u8			*pc_cpu_addr;
	dma_addr_t		pc_dma_addr;
	size_t			pc_free_start;
};

static struct rpcrdma_pool_chunk *
rpcrdma_pool_chunk_create(struct rpcrdma_pool *pool, gfp_t flags)
{
	struct rpcrdma_pool_chunk *chunk;

	chunk = kmalloc(sizeof(*chunk), flags);
	if (!chunk)
		return NULL;
	chunk->pc_cpu_addr = kmalloc_node(pool->rp_chunksize, flags,
					  ibdev_to_node(pool->rp_device));
	if (!chunk->pc_cpu_addr) {
		kfree(chunk);
		return NULL;
	}
	chunk->pc_dma_addr = ib_dma_map_single(pool->rp_device,
					       chunk->pc_cpu_addr,
					       pool->rp_chunksize,
					       pool->rp_direction);
	if (ib_dma_mapping_error(pool->rp_device, chunk->pc_dma_addr)) {
		kfree(chunk->pc_cpu_addr);
		kfree(chunk);
		return NULL;
	}

	chunk->pc_free_start = 0;
	return chunk;
}

/**
 * rpcrdma_pool_create - Initialize a buffer pool
 * @args: pool creation arguments
 * @flags: GFP flags for pool creation
 *
 * Returns a pointer to an opaque rpcrdma_pool object or
 * NULL. If a pool object is returned, caller must free the
 * returned object using rpcrdma_pool_destroy().
 */
struct rpcrdma_pool *
rpcrdma_pool_create(struct rpcrdma_pool_args *args, gfp_t flags)
{
	struct rpcrdma_pool *pool;

	pool = kmalloc(sizeof(*pool), flags);
	if (!pool)
		return NULL;

	INIT_LIST_HEAD(&pool->rp_chunk_list);
	pool->rp_device = args->pa_device;
	pool->rp_chunksize = RPCRDMA_MAX_INLINE_THRESH;
	pool->rp_bufsize = args->pa_bufsize;
	pool->rp_direction = args->pa_direction;
	return pool;
}

/**
 * rpcrdma_pool_destroy - Release resources owned by a buffer pool
 * @pool: buffer pool object that will no longer be used
 */
void
rpcrdma_pool_destroy(struct rpcrdma_pool *pool)
{
	struct rpcrdma_pool_chunk *chunk;

	while (!list_empty(&pool->rp_chunk_list)) {
		chunk = list_first_entry(&pool->rp_chunk_list,
					 struct rpcrdma_pool_chunk,
					 pc_next_chunk);
		list_del(&chunk->pc_next_chunk);
		ib_dma_unmap_single(pool->rp_device, chunk->pc_dma_addr,
				    pool->rp_chunksize, pool->rp_direction);
		kfree(chunk->pc_cpu_addr);
		kfree(chunk);
	}
	kfree(pool);
}

static struct rpcrdma_pool_chunk *
rpcrdma_pool_find_chunk(struct rpcrdma_pool *pool, gfp_t flags)
{
	struct rpcrdma_pool_chunk *chunk;

	list_for_each_entry(chunk, &pool->rp_chunk_list, pc_next_chunk) {
		size_t remaining = pool->rp_chunksize - chunk->pc_free_start;

		if (pool->rp_bufsize >= remaining)
			return chunk;
	}

	chunk = rpcrdma_pool_chunk_create(pool, flags);
	if (chunk)
		list_add(&chunk->pc_next_chunk, &pool->rp_chunk_list);
	return chunk;
}

/**
 * rpcrdma_pool_alloc_buffer - Allocate a buffer from a pool
 * @pool: buffer pool from which to allocate the buffer
 * @flags: GFP flags for the allocation
 * @cpu_addr: CPU address of the buffer
 * @dma_addr: mapped DMA address of the buffer
 *
 * Return values:
 *   %true: @cpu_addr and @dma_addr are filled in with a DMA-mapped buffer
 *   %false: No buffer is available
 *
 * When successful, the returned buffer is freed automatically when the
 * buffer pool is released by rpcrdma_pool_destroy().
 */
bool
rpcrdma_pool_alloc_buffer(struct rpcrdma_pool *pool, gfp_t flags,
			  void **cpu_addr, dma_addr_t *dma_addr)
{
	struct rpcrdma_pool_chunk *chunk;

	chunk = rpcrdma_pool_find_chunk(pool, flags);
	if (!chunk)
		return false;

	*cpu_addr = chunk->pc_cpu_addr + chunk->pc_free_start;
	*dma_addr = chunk->pc_dma_addr + chunk->pc_free_start;
	chunk->pc_free_start += pool->rp_bufsize;
	return true;
}
