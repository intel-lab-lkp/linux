/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Oracle and/or its affiliates.
 *
 * Pools for RDMA Receive buffers.
 */

#ifndef RPCRDMA_POOL_H
#define RPCRDMA_POOL_H

struct rpcrdma_pool;

struct rpcrdma_pool *rpcrdma_pool_create(struct ib_pd *pd, size_t bufsize,
					 gfp_t flags);
void rpcrdma_pool_destroy(struct rpcrdma_pool *pool);
bool rpcrdma_pool_buffer_alloc(struct rpcrdma_pool *pool, gfp_t flags,
			       void **cpu_addr, struct ib_sge *sge);
void rpcrdma_pool_buffer_sync(struct rpcrdma_pool *pool, struct ib_sge *sge);

#endif /* RPCRDMA_POOL_H */
