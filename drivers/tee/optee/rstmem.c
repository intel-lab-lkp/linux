// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Linaro Limited
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dma-map-ops.h>
#include <linux/errno.h>
#include <linux/genalloc.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tee_core.h>
#include <linux/types.h>
#include "optee_private.h"

int optee_rstmem_alloc(struct tee_context *ctx, struct tee_shm *shm,
		       u32 flags, u32 use_case, size_t size)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct tee_shm_pool *pool;

	if (!optee->rstmem_pools)
		return -EINVAL;
	if (flags)
		return -EINVAL;

	pool = xa_load(&optee->rstmem_pools->xa, use_case);
	if (!pool)
		return -EINVAL;

	return pool->ops->alloc(pool, shm, size, 0);
}

void optee_rstmem_free(struct tee_context *ctx, struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct tee_shm_pool *pool;

	pool = xa_load(&optee->rstmem_pools->xa, shm->use_case);
	if (pool)
		pool->ops->free(pool, shm);
	else
		pr_err("Can't find pool for use_case %u\n", shm->use_case);
}

int optee_rstmem_pools_init(struct optee *optee)
{
	struct optee_rstmem_pools *pools;

	pools = kmalloc(sizeof(*pools), GFP_KERNEL);
	if (!pools)
		return -ENOMEM;

	mutex_init(&pools->mutex);
	xa_init(&pools->xa);
	optee->rstmem_pools = pools;
	return 0;
}

void optee_rstmem_pools_uninit(struct optee *optee)
{
	if (optee->rstmem_pools) {
		struct tee_shm_pool *pool;
		u_long idx;

		xa_for_each(&optee->rstmem_pools->xa, idx, pool) {
			xa_erase(&optee->rstmem_pools->xa, idx);
			pool->ops->destroy_pool(pool);
		}

		xa_destroy(&optee->rstmem_pools->xa);
		mutex_destroy(&optee->rstmem_pools->mutex);
		kfree(optee->rstmem_pools);
		optee->rstmem_pools = NULL;
	}
}
