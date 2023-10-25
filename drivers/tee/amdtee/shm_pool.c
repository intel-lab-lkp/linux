// SPDX-License-Identifier: MIT
/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 */

#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/psp.h>
#include "amdtee_private.h"

static int pool_op_alloc(struct tee_shm_pool *pool, struct tee_shm *shm,
			 size_t size, size_t align)
{
	unsigned int order = get_order(size);
	int rc;

	shm->size = PAGE_SIZE << order;

	/* Allocate and map memory in to TEE */
	rc = amdtee_alloc_shmem(shm);
	if (rc) {
		shm->kaddr = NULL;
		return rc;
	}

	return 0;
}

static void pool_op_free(struct tee_shm_pool *pool, struct tee_shm *shm)
{
	/* Unmap the shared memory from TEE */
	amdtee_free_shmem(shm);

	shm->size = 0;
	shm->kaddr = NULL;
}

static void pool_op_destroy_pool(struct tee_shm_pool *pool)
{
	kfree(pool);
}

static const struct tee_shm_pool_ops pool_ops = {
	.alloc = pool_op_alloc,
	.free = pool_op_free,
	.destroy_pool = pool_op_destroy_pool,
};

struct tee_shm_pool *amdtee_config_shm(void)
{
	struct tee_shm_pool *pool = kzalloc(sizeof(*pool), GFP_KERNEL);

	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->ops = &pool_ops;

	return pool;
}
