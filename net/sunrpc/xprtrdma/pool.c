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
 *  - Buffers in one rpcrdma_pool instance are automatically released
 *    when the pool instance is destroyed
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
	struct ib_pd		*rp_pd;
	size_t			rp_shardsize;	// in bytes
	size_t			rp_bufsize;	// in bytes
	unsigned int		rp_bufs_per_shard;
	unsigned int		rp_pool_id;
};

struct rpcrdma_pool_shard {
	struct page		*pc_pages;
	u8			*pc_cpu_addr;
	u64			pc_mapped_addr;
	unsigned long		*pc_bitmap;
};

/*
 * For good NUMA awareness, ensure that the shard is allocated on
 * the NUMA node that the underlying device is affined to.
 *
 * For the shard buffer, we really want alloc_pages_node rather
 * than kmalloc_node.
 */
static struct rpcrdma_pool_shard *
rpcrdma_pool_shard_alloc(struct rpcrdma_pool *pool, gfp_t flags)
{
	struct ib_device *device = pool->rp_pd->device;
	int numa_node = ibdev_to_node(device);
	struct rpcrdma_pool_shard *shard;
	size_t bmap_size;

	shard = kmalloc_node(sizeof(*shard), flags, numa_node);
	if (!shard)
		goto fail;

	bmap_size = BITS_TO_LONGS(pool->rp_bufs_per_shard) * sizeof(unsigned long);
	shard->pc_bitmap = kzalloc(bmap_size, flags);
	if (!shard->pc_bitmap)
		goto free_shard;

	shard->pc_pages = alloc_pages_node(numa_node, flags,
					   get_order(pool->rp_shardsize));
	if (!shard->pc_pages)
		goto free_bitmap;

	shard->pc_cpu_addr = page_address(shard->pc_pages);
	shard->pc_mapped_addr = ib_dma_map_single(device, shard->pc_cpu_addr,
						  pool->rp_shardsize,
						  DMA_FROM_DEVICE);
	if (ib_dma_mapping_error(device, shard->pc_mapped_addr))
		goto free_iobuf;

	return shard;

free_iobuf:
	__free_pages(shard->pc_pages, get_order(pool->rp_shardsize));
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
	ib_dma_unmap_single(pool->rp_pd->device, shard->pc_mapped_addr,
			    pool->rp_shardsize, DMA_FROM_DEVICE);

	__free_pages(shard->pc_pages, get_order(pool->rp_shardsize));
	kfree(shard->pc_bitmap);
	kfree(shard);
}

/**
 * rpcrdma_pool_create - Allocate an rpcrdma_pool instance
 * @pd: RDMA protection domain to be used for the pool's buffers
 * @bufsize: Size, in bytes, of all buffers in the pool
 * @flags: GFP flags to be used during pool creation
 *
 * Returns a pointer to an opaque rpcrdma_pool instance, or NULL. If
 * a pool instance is returned, caller must free the instance using
 * rpcrdma_pool_destroy().
 */
struct rpcrdma_pool *rpcrdma_pool_create(struct ib_pd *pd, size_t bufsize,
					 gfp_t flags)
{
	struct rpcrdma_pool *pool;

	pool = kmalloc(sizeof(*pool), flags);
	if (!pool)
		return NULL;

	xa_init_flags(&pool->rp_xa, XA_FLAGS_ALLOC);
	pool->rp_pd = pd;
	pool->rp_shardsize = RPCRDMA_MAX_INLINE_THRESH;
	pool->rp_bufsize = bufsize;
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
 * @sge: OUT: an initialized scatter-gather entry
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
			  void **cpu_addr, struct ib_sge *sge)
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
	sge->addr = returned_mapped_addr;
	sge->length = pool->rp_bufsize;
	sge->lkey = pool->rp_pd->local_dma_lkey;

	trace_rpcrdma_pool_buffer(pool->rp_pool_id, returned_cpu_addr);
	return true;
}

/**
 * rpcrdma_pool_buffer_sync - Sync the contents of a pool buffer after I/O
 * @pool: buffer pool to which the buffer belongs
 * @sge: SGE containing the DMA-mapped buffer address and length
 */
void rpcrdma_pool_buffer_sync(struct rpcrdma_pool *pool, struct ib_sge *sge)
{
	ib_dma_sync_single_for_cpu(pool->rp_pd->device, sge->addr,
				   sge->length, DMA_FROM_DEVICE);
}
