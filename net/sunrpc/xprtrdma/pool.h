/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Oracle and/or its affiliates.
 *
 * Pools for Send and Receive buffers.
 */

#ifndef RPCRDMA_POOL_H
#define RPCRDMA_POOL_H

struct rpcrdma_pool_args {
	struct ib_device	*pa_device;
	size_t			pa_bufsize;
	enum dma_data_direction	pa_direction;
};

struct rpcrdma_pool;

struct rpcrdma_pool *
rpcrdma_pool_create(struct rpcrdma_pool_args *args, gfp_t flags);
void rpcrdma_pool_destroy(struct rpcrdma_pool *pool);
bool rpcrdma_pool_alloc_buffer(struct rpcrdma_pool *pool, gfp_t flags,
			       void **cpu_addr, dma_addr_t *dma_addr);

#endif /* RPCRDMA_POOL_H */
