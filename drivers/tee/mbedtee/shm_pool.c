// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Linaro Limited
 * Copyright (c) 2017, EPAM Systems
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 */
#include <linux/device.h>
#include <linux/genalloc.h>
#include <linux/slab.h>

#include "mbedtee_drv.h"

static int pool_op_alloc(struct tee_shm_pool *pool,
			 struct tee_shm *shm, size_t size, size_t align)
{
	unsigned int order = get_order(size);
	struct page *page;
	int rc = 0;

	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	shm->kaddr = page_address(page);
	shm->paddr = page_to_phys(page);
	shm->size = PAGE_SIZE << order;

	if (!(shm->flags & TEE_SHM_PRIV)) {
		struct mbedtee_device *mbedtee = tee_get_drvdata(shm->ctx->teedev);

		if (mbedtee->yield) {
			unsigned int nr_pages = 1 << order;
			unsigned int i;
			struct page **pages;

			pages = kmalloc_array(nr_pages, sizeof(*pages), GFP_KERNEL);
			if (!pages) {
				rc = -ENOMEM;
				goto err_pages;
			}

			for (i = 0; i < nr_pages; i++)
				pages[i] = page + i;

			rc = mbedtee_shm_register(shm->ctx, shm, pages,
						  nr_pages,
						  (unsigned long)shm->kaddr);
			kfree(pages);
			if (rc)
				goto err_pages;
		}
	}

	return 0;

err_pages:
	free_pages((unsigned long)shm->kaddr, get_order(shm->size));
	shm->kaddr = NULL;
	return rc;
}

static void pool_op_free(struct tee_shm_pool *pool,
			 struct tee_shm *shm)
{
	if (!(shm->flags & TEE_SHM_PRIV)) {
		struct mbedtee_device *mbedtee = tee_get_drvdata(shm->ctx->teedev);

		if (mbedtee->yield && shm->sec_world_id)
			mbedtee_shm_unregister(shm->ctx, shm);
	}

	free_pages((unsigned long)shm->kaddr, get_order(shm->size));
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

/**
 * mbedtee_shm_pool_alloc_pages() - create page-based allocator pool
 *
 * This pool is used when MbedTEE supports dynamic shared memory. Command
 * buffers and similar structures are allocated from kernel's own memory.
 *
 * Return: pointer to a tee_shm_pool or ERR_PTR on failure
 */
struct tee_shm_pool *mbedtee_shm_pool_alloc_pages(void)
{
	struct tee_shm_pool *pool = kzalloc_obj(*pool, GFP_KERNEL);

	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->ops = &pool_ops;

	return pool;
}
