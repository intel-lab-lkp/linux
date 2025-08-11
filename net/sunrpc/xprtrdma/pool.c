// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Oracle and/or its affiliates.
 *
 * Pools for RPC-over-RDMA Receive buffers.
 *
 * A buffer pool attempts to conserve both the number of DMA mappings
 * and the device's IOVA space by collecting small buffers together
 * into a shard that has a single DMA mapping.
 *
 * API Contract:
 *  - Buffers contained in one rpcrdma_pool instance are the same
 *    size (rp_bufsize), no larger than RPCRDMA_MAX_INLINE_THRESH
 *  - Buffers in one rpcrdma_pool instance are mapped using the same
 *    DMA direction
 *  - Buffers in one rpcrdma_pool instance are automatically released
 *    when the instance is destroyed
 *
 * Future work:
 *   - Manage pool resources by reference count
 */

#include <linux/list.h>
#include <linux/xarray.h>
#include <linux/sunrpc/svc_rdma.h>

#include <rdma/ib_verbs.h>

#include "xprt_rdma.h"
#include "pool.h"

#include <trace/events/rpcrdma.h>

/*
 * An idr would give near perfect pool ID uniqueness, but for
 * the moment the pool ID is used only for observability, not
 * correctness.
 */
static atomic_t rpcrdma_pool_id;

struct rpcrdma_pool {
	struct xarray		rp_xa;
	struct ib_device	*rp_device;
	size_t			rp_shardsize;	// in bytes
	size_t			rp_bufsize;	// in bytes
	enum dma_data_direction	rp_direction;
	unsigned int		rp_bufs_per_shard;
	unsigned int		rp_pool_id;
};

struct rpcrdma_pool_shard {
	u8			*pc_cpu_addr;
	u64			pc_mapped_addr;
	unsigned long		*pc_bitmap;
};

static struct rpcrdma_pool_shard *
rpcrdma_pool_shard_alloc(struct rpcrdma_pool *pool, gfp_t flags)
{
	struct rpcrdma_pool_shard *shard;
	size_t bmap_size;

	shard = kmalloc(sizeof(*shard), flags);
	if (!shard)
		goto fail;

	bmap_size = BITS_TO_LONGS(pool->rp_bufs_per_shard) * sizeof(unsigned long);
	shard->pc_bitmap = kzalloc(bmap_size, flags);
	if (!shard->pc_bitmap)
		goto free_shard;

	/*
	 * For good NUMA awareness, allocate the shard's I/O buffer
	 * on the NUMA node that the underlying device is affined to.
	 */
	shard->pc_cpu_addr = kmalloc_node(pool->rp_shardsize, flags,
					  ibdev_to_node(pool->rp_device));
	if (!shard->pc_cpu_addr)
		goto free_bitmap;
	shard->pc_mapped_addr = ib_dma_map_single(pool->rp_device,
						  shard->pc_cpu_addr,
						  pool->rp_shardsize,
						  pool->rp_direction);
	if (ib_dma_mapping_error(pool->rp_device, shard->pc_mapped_addr))
		goto free_iobuf;

	return shard;

free_iobuf:
	kfree(shard->pc_cpu_addr);
free_bitmap:
	kfree(shard->pc_bitmap);
free_shard:
	kfree(shard);
fail:
	return NULL;
}

static void
rpcrdma_pool_shard_free(struct rpcrdma_pool *pool,
			struct rpcrdma_pool_shard *shard)
{
	ib_dma_unmap_single(pool->rp_device, shard->pc_mapped_addr,
			    pool->rp_shardsize, pool->rp_direction);
	kfree(shard->pc_cpu_addr);
	kfree(shard->pc_bitmap);
	kfree(shard);
}

/**
 * rpcrdma_pool_create - Allocate and initialize an rpcrdma_pool instance
 * @args: pool creation arguments
 * @flags: GFP flags used during pool creation
 *
 * Returns a pointer to an opaque rpcrdma_pool instance or
 * NULL. If a pool instance is returned, caller must free the
 * returned instance using rpcrdma_pool_destroy().
 */
struct rpcrdma_pool *
rpcrdma_pool_create(struct rpcrdma_pool_args *args, gfp_t flags)
{
	struct rpcrdma_pool *pool;

	pool = kmalloc(sizeof(*pool), flags);
	if (!pool)
		return NULL;

	xa_init_flags(&pool->rp_xa, XA_FLAGS_ALLOC);
	pool->rp_device = args->pa_device;
	pool->rp_shardsize = RPCRDMA_MAX_INLINE_THRESH;
	pool->rp_bufsize = args->pa_bufsize;
	pool->rp_direction = args->pa_direction;
	pool->rp_bufs_per_shard = pool->rp_shardsize / pool->rp_bufsize;
	pool->rp_pool_id = atomic_inc_return(&rpcrdma_pool_id);

	trace_rpcrdma_pool_create(pool->rp_pool_id, pool->rp_bufsize);
	return pool;
}

/**
 * rpcrdma_pool_destroy - Release resources owned by @pool
 * @pool: buffer pool instance that will no longer be used
 *
 * This call releases all buffers in @pool that were allocated
 * via rpcrdma_pool_buffer_alloc().
 */
void
rpcrdma_pool_destroy(struct rpcrdma_pool *pool)
{
	struct rpcrdma_pool_shard *shard;
	unsigned long index;

	trace_rpcrdma_pool_destroy(pool->rp_pool_id);

	xa_for_each(&pool->rp_xa, index, shard) {
		trace_rpcrdma_pool_shard_free(pool->rp_pool_id, index);
		xa_erase(&pool->rp_xa, index);
		rpcrdma_pool_shard_free(pool, shard);
	}

	xa_destroy(&pool->rp_xa);
	kfree(pool);
}

/**
 * rpcrdma_pool_buffer_alloc - Allocate a buffer from @pool
 * @pool: buffer pool from which to allocate the buffer
 * @flags: GFP flags used during this allocation
 * @cpu_addr: CPU address of the buffer
 * @mapped_addr: mapped address of the buffer
 *
 * Return values:
 *   %true: @cpu_addr and @mapped_addr are filled in with a DMA-mapped buffer
 *   %false: No buffer is available
 *
 * When rpcrdma_pool_buffer_alloc() is successful, the returned
 * buffer is freed automatically when the buffer pool is released
 * by rpcrdma_pool_destroy().
 */
bool
rpcrdma_pool_buffer_alloc(struct rpcrdma_pool *pool, gfp_t flags,
			  void **cpu_addr, u64 *mapped_addr)
{
	struct rpcrdma_pool_shard *shard;
	u64 returned_mapped_addr;
	void *returned_cpu_addr;
	unsigned long index;
	u32 id;

	xa_for_each(&pool->rp_xa, index, shard) {
		unsigned int i;

		returned_cpu_addr = shard->pc_cpu_addr;
		returned_mapped_addr = shard->pc_mapped_addr;
		for (i = 0; i < pool->rp_bufs_per_shard; i++) {
			if (!test_and_set_bit(i, shard->pc_bitmap)) {
				returned_cpu_addr += i * pool->rp_bufsize;
				returned_mapped_addr += i * pool->rp_bufsize;
				goto out;
			}
		}
	}

	shard = rpcrdma_pool_shard_alloc(pool, flags);
	if (!shard)
		return false;
	set_bit(0, shard->pc_bitmap);
	returned_cpu_addr = shard->pc_cpu_addr;
	returned_mapped_addr = shard->pc_mapped_addr;

	if (xa_alloc(&pool->rp_xa, &id, shard, xa_limit_16b, flags) != 0) {
		rpcrdma_pool_shard_free(pool, shard);
		return false;
	}
	trace_rpcrdma_pool_shard_new(pool->rp_pool_id, id);

out:
	*cpu_addr = returned_cpu_addr;
	*mapped_addr = returned_mapped_addr;

	trace_rpcrdma_pool_buffer(pool->rp_pool_id, returned_cpu_addr);
	return true;
}
