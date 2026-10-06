// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/overflow.h>
#include <linux/rpmi_tee.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include "optee_private.h"
#include "optee_rpmi.h"
#include "optee_rpc_cmd.h"

/* Nonzero nonce keeps parcel ID zero distinct from a null reference. */
#define OPTEE_RPMI_SHM_NONCE	1

struct optee_rpmi_parcel_key {
	u32 parcel_id;
	u32 nonce;
};

struct optee_rpmi_shm_rht_entry {
	struct rhash_head node;
	struct optee_rpmi_parcel_key key;
	struct tee_shm *shm;
};

static const struct rhashtable_params optee_rpmi_shm_rht_params = {
	.head_offset = offsetof(struct optee_rpmi_shm_rht_entry, node),
	.key_offset = offsetof(struct optee_rpmi_shm_rht_entry, key),
	.key_len = sizeof(struct optee_rpmi_parcel_key),
	.automatic_shrinking = true,
};

/* Keep transport errors separate from the control status in a response. */
static int optee_rpmi_call_with_status(struct optee *optee,
				       const void *req, size_t req_len,
				       void *resp, size_t resp_size,
				       s32 *status)
{
	struct rpmi_tee_device *rdev = optee->rpmi.rdev;
	size_t received = resp_size;
	int ret;

	ret = rdev->ops->msg_ops->call(rdev, req, req_len, resp, &received);
	if (ret)
		return ret;

	if (received != resp_size)
		return -EPROTO;

	*status = get_unaligned_le32(resp);

	return 0;
}

/**
 * optee_rpmi_call - Send a control request and decode its RPMI status
 * @optee: OP-TEE instance.
 * @req: Control request, including the operation number.
 * @req_len: Request size in bytes.
 * @resp: Response buffer, beginning with a little-endian RPMI status.
 * @resp_size: Exact expected response size, including the status field.
 *
 * Return: 0 on success, a transport error, -EPROTO for an unexpected response
 * size, or the control status converted to a Linux error code.
 */
static int optee_rpmi_call(struct optee *optee, const void *req, size_t req_len,
			   void *resp, size_t resp_size)
{
	s32 status;
	int ret;

	ret = optee_rpmi_call_with_status(optee, req, req_len, resp, resp_size,
					  &status);
	if (ret)
		return ret;

	return rpmi_to_linux_error(status);
}

static int optee_rpmi_shm_rht_init(struct optee *optee)
{
	int ret;

	mutex_init(&optee->rpmi.shm_rht_lock);
	ret = rhashtable_init(&optee->rpmi.shm_rht, &optee_rpmi_shm_rht_params);
	if (ret)
		mutex_destroy(&optee->rpmi.shm_rht_lock);

	return ret;
}

static void optee_rpmi_shm_rht_free(void *ptr, void *arg)
{
	kfree(ptr);
}

static void optee_rpmi_shm_rht_uninit(struct optee *optee)
{
	rhashtable_free_and_destroy(&optee->rpmi.shm_rht,
				    optee_rpmi_shm_rht_free, NULL);
	mutex_destroy(&optee->rpmi.shm_rht_lock);
}

/* Allocate and publish a parcel-to-SHM mapping. */
static int optee_rpmi_shm_rht_add(struct optee *optee, struct tee_shm *shm,
				  u32 parcel_id, u32 nonce)
{
	struct optee_rpmi_shm_rht_entry *entry;
	int ret;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;

	entry->shm = shm;
	entry->key.parcel_id = parcel_id;
	entry->key.nonce = nonce;

	scoped_guard(mutex, &optee->rpmi.shm_rht_lock)
		ret = rhashtable_lookup_insert_fast(&optee->rpmi.shm_rht,
						    &entry->node,
						    optee_rpmi_shm_rht_params);
	if (ret)
		kfree(entry);

	return ret;
}

/* Remove and free a parcel-to-SHM mapping. */
static int optee_rpmi_shm_rht_rm(struct optee *optee, u32 parcel_id, u32 nonce)
{
	struct optee_rpmi_shm_rht_entry *entry;
	struct optee_rpmi_parcel_key key = {
		.parcel_id = parcel_id,
		.nonce = nonce,
	};
	int ret = -ENOENT;

	scoped_guard(mutex, &optee->rpmi.shm_rht_lock) {
		entry = rhashtable_lookup_fast(&optee->rpmi.shm_rht, &key,
					       optee_rpmi_shm_rht_params);
		if (entry)
			ret = rhashtable_remove_fast(&optee->rpmi.shm_rht,
						     &entry->node,
						     optee_rpmi_shm_rht_params);
	}

	if (!ret)
		kfree(entry);

	return ret;
}

/* Return a raw pointer; the surrounding call or RPC owns the SHM lifetime. */
static struct tee_shm *
optee_rpmi_get_shm_for_parcel(struct optee *optee, u32 parcel_id, u32 nonce)
{
	struct optee_rpmi_shm_rht_entry *entry;
	struct optee_rpmi_parcel_key key = {
		.parcel_id = parcel_id,
		.nonce = nonce,
	};

	guard(mutex)(&optee->rpmi.shm_rht_lock);
	entry = rhashtable_lookup_fast(&optee->rpmi.shm_rht, &key,
				       optee_rpmi_shm_rht_params);

	return entry ? entry->shm : NULL;
}

/* Extract the parcel ID and nonce stored in the SHM identity. */
static void optee_rpmi_shm_get_identity(const struct tee_shm *shm,
					u32 *parcel_id, u32 *nonce)
{
	*parcel_id = lower_32_bits(shm->sec_world_id);
	*nonce = upper_32_bits(shm->sec_world_id);
}

static int optee_rpmi_shm_register(struct tee_context *ctx, struct tee_shm *shm,
				   struct page **pages, size_t num_pages,
				   unsigned long start)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct rpmi_tee_device *rdev = optee->rpmi.rdev;
	struct rpmi_tee_mem_receiver receiver = {
		.endpoint_id = rdev->endpoint_id,
		.access = RPMI_TEE_MEM_ACCESS_READ | RPMI_TEE_MEM_ACCESS_WRITE,
	};
	struct rpmi_tee_mem_args args = {
		.nonce = OPTEE_RPMI_SHM_NONCE,
		.receivers = &receiver,
		.receiver_count = 1,
		.creator_access = RPMI_TEE_MEM_ACCESS_READ |
				  RPMI_TEE_MEM_ACCESS_WRITE,
	};
	struct sg_table sgt;
	int ret;

	ret = optee_check_mem_type(start, num_pages);
	if (ret)
		return ret;

	ret = sg_alloc_table_from_pages(&sgt, pages, num_pages, 0,
					num_pages * PAGE_SIZE, GFP_KERNEL);
	if (ret)
		return ret;

	args.sg = sgt.sgl;
	ret = rdev->ops->mem_ops->memory_share(rdev, &args);
	sg_free_table(&sgt);
	if (ret)
		return ret;

	ret = optee_rpmi_shm_rht_add(optee, shm, args.parcel_id, args.nonce);
	if (ret) {
		int reclaim_ret;

		reclaim_ret = rdev->ops->mem_ops->memory_reclaim(rdev, args.parcel_id);
		if (reclaim_ret)
			dev_err(&rdev->dev, "reclaim parcel %#x failed: %d\n",
				args.parcel_id, reclaim_ret);
		return ret;
	}

	shm->sec_world_id = ((u64)args.nonce << 32) | args.parcel_id;

	return 0;
}

static int optee_rpmi_shm_unregister(struct tee_context *ctx,
				     struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct rpmi_tee_device *rdev = optee->rpmi.rdev;
	struct optee_rpmi_unregister_req req;
	struct optee_rpmi_status_resp resp;
	u32 parcel_id, nonce;
	int ret;

	optee_rpmi_shm_get_identity(shm, &parcel_id, &nonce);
	optee_rpmi_shm_rht_rm(optee, parcel_id, nonce);
	shm->sec_world_id = 0;

	req.op = cpu_to_le32(OPTEE_RPMI_UNREGISTER_SHM);
	req.parcel_id = cpu_to_le32(parcel_id);
	req.nonce = cpu_to_le32(nonce);
	ret = optee_rpmi_call(optee, &req, sizeof(req), &resp, sizeof(resp));
	if (ret)
		dev_err(&rdev->dev, "unregister parcel %#x failed: %d\n",
			parcel_id, ret);

	ret = rdev->ops->mem_ops->memory_reclaim(rdev, parcel_id);
	if (ret)
		dev_err(&rdev->dev, "reclaim parcel %#x failed: %d\n",
			parcel_id, ret);

	return ret;
}

static int optee_rpmi_shm_unregister_supp(struct tee_context *ctx,
					  struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct rpmi_tee_device *rdev = optee->rpmi.rdev;
	u32 parcel_id, nonce;
	int ret;

	optee_rpmi_shm_get_identity(shm, &parcel_id, &nonce);
	optee_rpmi_shm_rht_rm(optee, parcel_id, nonce);
	shm->sec_world_id = 0;
	/* OP-TEE has already retired the parcel through SHM_FREE RPC. */
	ret = rdev->ops->mem_ops->memory_reclaim(rdev, parcel_id);
	if (ret)
		dev_err(&rdev->dev, "reclaim parcel %#x failed: %d\n",
			parcel_id, ret);

	return ret;
}

static int optee_rpmi_pool_alloc(struct tee_shm_pool *pool, struct tee_shm *shm,
				 size_t size, size_t align)
{
	return tee_dyn_shm_alloc_helper(shm, size, align,
					optee_rpmi_shm_register);
}

static void optee_rpmi_pool_free(struct tee_shm_pool *pool, struct tee_shm *shm)
{
	tee_dyn_shm_free_helper(shm, optee_rpmi_shm_unregister);
}

static void optee_rpmi_pool_destroy(struct tee_shm_pool *pool)
{
	kfree(pool);
}

static const struct tee_shm_pool_ops optee_rpmi_pool_ops = {
	.alloc = optee_rpmi_pool_alloc,
	.free = optee_rpmi_pool_free,
	.destroy_pool = optee_rpmi_pool_destroy,
};

static struct tee_shm_pool *optee_rpmi_shm_pool_alloc(void)
{
	struct tee_shm_pool *pool;

	pool = kzalloc_obj(*pool);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->ops = &optee_rpmi_pool_ops;

	return pool;
}

/* Convert a memory reference to an OP-TEE RPMI parcel reference. */
static int to_msg_param_rpmi_mem(struct optee_msg_param *mp,
				 const struct tee_param *p)
{
	struct tee_shm *shm = p->u.memref.shm;

	mp->attr = OPTEE_MSG_ATTR_TYPE_PMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	memset(&mp->u, 0, sizeof(mp->u));
	/* For !shm, return parcel_id = 0 and nonce = 0 to represent NULL. */
	if (shm) {
		if (check_add_overflow((u64)shm->offset,
				       (u64)p->u.memref.shm_offs,
				       &mp->u.pmem.offs))
			return -EINVAL;

		optee_rpmi_shm_get_identity(shm, &mp->u.pmem.parcel_id,
					    &mp->u.pmem.nonce);
	}

	mp->u.pmem.size = p->u.memref.size;

	return 0;
}

static int optee_rpmi_to_msg_param(struct optee *optee,
				   struct optee_msg_param *msg_params,
				   size_t num_params,
				   const struct tee_param *params)
{
	size_t n;

	for (n = 0; n < num_params; n++) {
		const struct tee_param *p = params + n;
		struct optee_msg_param *mp = msg_params + n;
		switch (p->attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_NONE:
			mp->attr = OPTEE_MSG_ATTR_TYPE_NONE;
			memset(&mp->u, 0, sizeof(mp->u));
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT:
			optee_to_msg_param_value(mp, p);
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
			if (to_msg_param_rpmi_mem(mp, p))
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

/* Convert an RPMI parcel reference to a memref; callers own SHM lifetime. */
static int from_msg_param_rpmi_mem(struct optee *optee, struct tee_param *p,
				   u32 attr, const struct optee_msg_param *mp)
{
	struct tee_shm *shm;
	u64 offset;

	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT + attr -
		  OPTEE_MSG_ATTR_TYPE_PMEM_INPUT;

	if (mp->u.pmem.size > SIZE_MAX)
		return -EOVERFLOW;
	p->u.memref.size = mp->u.pmem.size;

	if (!mp->u.pmem.nonce) {
		/* Return NULL shm. */
		if (mp->u.pmem.offs || mp->u.pmem.parcel_id)
			return -EINVAL;
		p->u.memref.shm = NULL;
		p->u.memref.shm_offs = 0;
		return 0;
	}

	shm = optee_rpmi_get_shm_for_parcel(optee, mp->u.pmem.parcel_id,
					    mp->u.pmem.nonce);
	if (!shm || mp->u.pmem.offs < shm->offset)
		return -EINVAL;

	offset = mp->u.pmem.offs - shm->offset;
	if (offset > SIZE_MAX)
		return -EOVERFLOW;

	p->u.memref.shm = shm;
	p->u.memref.shm_offs = offset;

	return 0;
}

static int optee_rpmi_from_msg_param(struct optee *optee,
				     struct tee_param *params,
				     size_t num_params,
				     const struct optee_msg_param *msg_params)
{
	size_t n;

	for (n = 0; n < num_params; n++) {
		const struct optee_msg_param *mp = msg_params + n;
		struct tee_param *p = params + n;
		u32 attr = mp->attr & OPTEE_MSG_ATTR_TYPE_MASK;
		int ret;

		switch (attr) {
		case OPTEE_MSG_ATTR_TYPE_NONE:
			p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
			memset(&p->u, 0, sizeof(p->u));
			break;
		case OPTEE_MSG_ATTR_TYPE_VALUE_INPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
			optee_from_msg_param_value(p, attr, mp);
			break;
		case OPTEE_MSG_ATTR_TYPE_PMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_PMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_PMEM_INOUT:
			ret = from_msg_param_rpmi_mem(optee, p, attr, mp);
			if (ret)
				return ret;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static void optee_rpmi_handle_rpc_shm_alloc(struct tee_context *ctx,
					    struct optee *optee,
					    struct optee_msg_arg *arg)
{
	u32 parcel_id, nonce;
	struct tee_shm *shm;
	u64 type, size;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)
		goto err_bad_param;

	type = arg->params[0].u.value.a;
	size = arg->params[0].u.value.b;
	if (!size || size > SIZE_MAX - PAGE_SIZE + 1)
		goto err_bad_param;

	switch (type) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		shm = optee_rpc_cmd_alloc_suppl(ctx, size);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
		shm = tee_shm_alloc_priv_buf(optee->ctx, size);
		break;
	default:
		goto err_bad_param;
	}

	if (IS_ERR(shm)) {
		arg->ret = TEEC_ERROR_OUT_OF_MEMORY;
		return;
	}

	optee_rpmi_shm_get_identity(shm, &parcel_id, &nonce);
	arg->params[0] = (struct optee_msg_param) {
		.attr = OPTEE_MSG_ATTR_TYPE_PMEM_OUTPUT,
		.u.pmem = {
			.offs = shm->offset,
			.size = tee_shm_get_size(shm),
			.parcel_id = parcel_id,
			.nonce = nonce,
		},
	};

	arg->ret = TEEC_SUCCESS;
	return;

err_bad_param:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

static void optee_rpmi_handle_rpc_shm_free(struct tee_context *ctx,
					   struct optee *optee,
					   struct optee_msg_arg *arg)
{
	struct tee_shm *shm;
	u64 type, parcel_id, nonce;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)
		goto err_bad_param;

	type = arg->params[0].u.value.a;
	parcel_id = arg->params[0].u.value.b;
	nonce = arg->params[0].u.value.c;
	if (parcel_id > U32_MAX || nonce > U32_MAX)
		goto err_bad_param;

	shm = optee_rpmi_get_shm_for_parcel(optee, parcel_id, nonce);
	if (!shm)
		goto err_bad_param;

	switch (type) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		optee_rpc_cmd_free_suppl(ctx, shm);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
		tee_shm_free(shm);
		break;
	default:
		goto err_bad_param;
	}

	arg->ret = TEEC_SUCCESS;
	return;

err_bad_param:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

/* OP-TEE leaves the shared arguments unchanged while Linux handles the RPC. */
static void optee_rpmi_handle_rpc_cmd(struct tee_context *ctx,
				      struct optee *optee,
				      struct optee_msg_arg *arg)
{
	arg->ret_origin = TEEC_ORIGIN_COMMS;
	switch (arg->cmd) {
	case OPTEE_RPC_CMD_SHM_ALLOC:
		optee_rpmi_handle_rpc_shm_alloc(ctx, optee, arg);
		break;
	case OPTEE_RPC_CMD_SHM_FREE:
		optee_rpmi_handle_rpc_shm_free(ctx, optee, arg);
		break;
	default:
		optee_rpc_cmd(ctx, optee, arg);
	}
}
