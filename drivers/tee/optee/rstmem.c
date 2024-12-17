// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Linaro Limited
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cma.h>
#include <linux/dma-map-ops.h>
#include <linux/errno.h>
#include <linux/genalloc.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tee_core.h>
#include <linux/types.h>
#include "optee_private.h"

#ifdef CONFIG_CMA
struct optee_rstmem_cma_pool {
	struct tee_shm_pool pool;
	struct page *page;
	struct optee *optee;
	size_t page_count;
	u16 *end_points;
	u_int end_point_count;
	u_int align;
	refcount_t refcount;
	struct tee_shm rstmem;
	/* Protects when initializing and tearing down this struct */
	struct mutex mutex;
};

static struct optee_rstmem_cma_pool *
to_rstmem_cma_pool(struct tee_shm_pool *pool)
{
	return container_of(pool, struct optee_rstmem_cma_pool, pool);
}

static int init_cma_rstmem(struct optee_rstmem_cma_pool *rp)
{
	struct cma *cma = dev_get_cma_area(&rp->optee->teedev->dev);
	int rc;

	rp->page = cma_alloc(cma, rp->page_count, rp->align, true/*no_warn*/);
	if (!rp->page)
		return -ENOMEM;

	/*
	 * TODO unmap the memory range since the physical memory will
	 * become inaccesible after the lend_rstmem() call.
	 */

	rp->rstmem.paddr = page_to_phys(rp->page);
	rp->rstmem.size = rp->page_count * PAGE_SIZE;
	rc = rp->optee->ops->lend_rstmem(rp->optee, &rp->rstmem,
					 rp->end_points, rp->end_point_count);
	if (rc)
		goto err_release;

	rp->pool.private_data = gen_pool_create(PAGE_SHIFT, -1);
	if (!rp->pool.private_data) {
		rc = -ENOMEM;
		goto err_reclaim;
	}

	rc = gen_pool_add(rp->pool.private_data, rp->rstmem.paddr,
			  rp->rstmem.size, -1);
	if (rc)
		goto err_free_pool;

	refcount_set(&rp->refcount, 1);
	return 0;

err_free_pool:
	gen_pool_destroy(rp->pool.private_data);
err_reclaim:
	rp->optee->ops->reclaim_rstmem(rp->optee, &rp->rstmem);
err_release:
	cma_release(cma, rp->page, rp->page_count);
	rp->rstmem.paddr = 0;
	rp->rstmem.size = 0;
	rp->rstmem.sec_world_id = 0;
	return rc;
}

static int get_cma_rstmem(struct optee_rstmem_cma_pool *rp)
{
	int rc = 0;

	if (!refcount_inc_not_zero(&rp->refcount)) {
		mutex_lock(&rp->mutex);
		if (rp->pool.private_data) {
			/*
			 * Another thread has already initialized the pool
			 * before us, or the pool was just about to be torn
			 * down. Either way we only need to increase the
			 * refcount and we're done.
			 */
			refcount_inc(&rp->refcount);
		} else {
			rc = init_cma_rstmem(rp);
		}
		mutex_unlock(&rp->mutex);
	}

	return rc;
}

static void release_cma_rstmem(struct optee_rstmem_cma_pool *rp)
{
	gen_pool_destroy(rp->pool.private_data);
	rp->optee->ops->reclaim_rstmem(rp->optee, &rp->rstmem);
	cma_release(dev_get_cma_area(&rp->optee->teedev->dev), rp->page,
		    rp->page_count);

	rp->pool.private_data = NULL;
	rp->page = NULL;
	rp->rstmem.paddr = 0;
	rp->rstmem.size = 0;
	rp->rstmem.sec_world_id = 0;
}

static void put_cma_rstmem(struct optee_rstmem_cma_pool *rp)
{
	if (refcount_dec_and_test(&rp->refcount)) {
		mutex_lock(&rp->mutex);
		if (rp->pool.private_data)
			release_cma_rstmem(rp);
		mutex_unlock(&rp->mutex);
	}
}

static int rstmem_pool_op_cma_alloc(struct tee_shm_pool *pool,
				    struct tee_shm *shm, size_t size,
				    size_t align)
{
	struct optee_rstmem_cma_pool *rp = to_rstmem_cma_pool(pool);
	size_t sz = ALIGN(size, PAGE_SIZE);
	phys_addr_t pa;
	int rc;

	rc = get_cma_rstmem(rp);
	if (rc)
		return rc;

	pa = gen_pool_alloc(rp->pool.private_data, sz);
	if (!pa) {
		put_cma_rstmem(rp);
		return -ENOMEM;
	}

	shm->size = sz;
	shm->paddr = pa;
	shm->offset = pa - page_to_phys(rp->page);
	shm->sec_world_id = rp->rstmem.sec_world_id;

	return 0;
}

static void rstmem_pool_op_cma_free(struct tee_shm_pool *pool,
				    struct tee_shm *shm)
{
	struct optee_rstmem_cma_pool *rp = to_rstmem_cma_pool(pool);

	gen_pool_free(rp->pool.private_data, shm->paddr, shm->size);
	shm->size = 0;
	shm->paddr = 0;
	shm->offset = 0;
	shm->sec_world_id = 0;
	put_cma_rstmem(rp);
}

static void pool_op_cma_destroy_pool(struct tee_shm_pool *pool)
{
	struct optee_rstmem_cma_pool *rp = to_rstmem_cma_pool(pool);

	mutex_destroy(&rp->mutex);
	kfree(rp);
}

static struct tee_shm_pool_ops rstmem_pool_ops_cma = {
	.alloc = rstmem_pool_op_cma_alloc,
	.free = rstmem_pool_op_cma_free,
	.destroy_pool = pool_op_cma_destroy_pool,
};

static int get_rstmem_config(struct optee *optee, u32 use_case,
			     size_t *min_size, u_int *min_align,
			     u16 *end_points, u_int *ep_count)
{
	struct tee_param params[2] = {
		[0] = {
			.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT,
			.u.value.a = use_case,
		},
		[1] = {
			.attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT,
		},
	};
	struct optee_shm_arg_entry *entry;
	struct tee_shm *shm_param = NULL;
	struct optee_msg_arg *msg_arg;
	struct tee_shm *shm;
	u_int offs;
	int rc;

	if (end_points && *ep_count) {
		params[1].u.memref.size = *ep_count * sizeof(*end_points);
		shm_param = tee_shm_alloc_priv_buf(optee->ctx,
						   params[1].u.memref.size);
		if (IS_ERR(shm_param))
			return PTR_ERR(shm_param);
		params[1].u.memref.shm = shm_param;
	}

	msg_arg = optee_get_msg_arg(optee->ctx, ARRAY_SIZE(params), &entry,
				    &shm, &offs);
	if (IS_ERR(msg_arg)) {
		rc = PTR_ERR(msg_arg);
		goto out_free_shm;
	}
	msg_arg->cmd = OPTEE_MSG_CMD_GET_RSTMEM_CONFIG;

	rc = optee->ops->to_msg_param(optee, msg_arg->params,
				      ARRAY_SIZE(params), params,
				      false /*!update_out*/);
	if (rc)
		goto out_free_msg;

	rc = optee->ops->do_call_with_arg(optee->ctx, shm, offs, false);
	if (rc)
		goto out_free_msg;
	if (msg_arg->ret && msg_arg->ret != TEEC_ERROR_SHORT_BUFFER) {
		rc = -EINVAL;
		goto out_free_msg;
	}

	rc = optee->ops->from_msg_param(optee, params, ARRAY_SIZE(params),
					msg_arg->params, true /*update_out*/);
	if (rc)
		goto out_free_msg;

	if (!msg_arg->ret && end_points &&
	    *ep_count < params[1].u.memref.size / sizeof(u16)) {
		rc = -EINVAL;
		goto out_free_msg;
	}

	*min_size = params[0].u.value.a;
	*min_align = params[0].u.value.b;
	*ep_count = params[1].u.memref.size / sizeof(u16);

	if (msg_arg->ret == TEEC_ERROR_SHORT_BUFFER) {
		rc = -ENOSPC;
		goto out_free_msg;
	}

	if (end_points)
		memcpy(end_points, tee_shm_get_va(shm_param, 0),
		       params[1].u.memref.size);

out_free_msg:
	optee_free_msg_arg(optee->ctx, entry, offs);
out_free_shm:
	if (shm_param)
		tee_shm_free(shm_param);
	return rc;
}

static struct tee_shm_pool *alloc_rstmem_pool(struct optee *optee, u32 use_case)
{
	struct optee_rstmem_cma_pool *rp;
	size_t min_size;
	int rc;

	rp = kzalloc(sizeof(*rp), GFP_KERNEL);
	if (!rp)
		return ERR_PTR(-ENOMEM);
	rp->rstmem.use_case = use_case;

	rc = get_rstmem_config(optee, use_case, &min_size, &rp->align, NULL,
			       &rp->end_point_count);
	if (rc) {
		if (rc != -ENOSPC)
			goto err;
		rp->end_points = kcalloc(rp->end_point_count,
					 sizeof(*rp->end_points), GFP_KERNEL);
		if (!rp->end_points) {
			rc = -ENOMEM;
			goto err;
		}
		rc = get_rstmem_config(optee, use_case, &min_size, &rp->align,
				       rp->end_points, &rp->end_point_count);
		if (rc)
			goto err_kfree_eps;
	}

	rp->pool.ops = &rstmem_pool_ops_cma;
	rp->optee = optee;
	rp->page_count = min_size / PAGE_SIZE;
	mutex_init(&rp->mutex);

	return &rp->pool;

err_kfree_eps:
	kfree(rp->end_points);
err:
	kfree(rp);
	return ERR_PTR(rc);
}
#else /*CONFIG_CMA*/
static struct tee_shm_pool *alloc_rstmem_pool(struct optee *optee __unused,
					      u32 use_case __unused)
{
	return ERR_PTR(-EINVAL);
}
#endif /*CONFIG_CMA*/

int optee_rstmem_alloc(struct tee_context *ctx, struct tee_shm *shm,
		       u32 flags, u32 use_case, size_t size)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct tee_shm_pool *pool;
	int rc;

	if (!optee->rstmem_pools)
		return -EINVAL;
	if (flags)
		return -EINVAL;

	pool = xa_load(&optee->rstmem_pools->xa, use_case);
	if (!pool) {
		pool = alloc_rstmem_pool(optee, use_case);
		if (IS_ERR(pool))
			return PTR_ERR(pool);
		rc = xa_insert(&optee->rstmem_pools->xa, use_case, pool,
			       GFP_KERNEL);
		if (rc) {
			pool->ops->destroy_pool(pool);
			return rc;
		}
	}

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
