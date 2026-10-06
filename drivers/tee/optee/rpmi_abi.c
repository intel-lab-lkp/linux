// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/overflow.h>
#include <linux/rpmi_tee.h>
#include <linux/sched.h>
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

/* Handle RPC command or interrupt returns from a yielding call. */
static void optee_rpmi_handle_rpc(struct tee_context *ctx, struct optee *optee,
				  u32 result, struct optee_msg_arg *arg)
{
	switch (result) {
	case OPTEE_RPMI_YIELDING_CALL_RETURN_RPC_CMD:
		optee_rpmi_handle_rpc_cmd(ctx, optee, arg);
		break;
	case OPTEE_RPMI_YIELDING_CALL_RETURN_INTERRUPT:
		break;
	default:
		pr_warn("Unknown RPC func 0x%x\n", result);
		break;
	}
}

/**
 * optee_rpmi_yielding_call() - submit and resume a yielding RPMI command
 * @ctx: calling context
 * @req: initial command request
 * @rpc_arg: shared RPC argument buffer
 * @system_thread: caller requests TEE system thread support
 *
 * Only RPMI_ERR_BUSY rejection of the initial command permits retry.
 *
 * Return: zero on completion, or a negative error.
 */
static int optee_rpmi_yielding_call(struct tee_context *ctx,
				    const struct optee_rpmi_call_req *req,
				    struct optee_msg_arg *rpc_arg,
				    bool system_thread)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct optee_rpmi_resume_req resume = {
		.op = cpu_to_le32(OPTEE_RPMI_YIELDING_CALL_RESUME),
		/* resume_token is nonzero after OP-TEE suspends the call. */
		.resume_token = 0,
	};
	struct optee_rpmi_call_resp resp;
	struct optee_call_waiter waiter;
	u32 result;
	s32 status;
	int ret;

	optee_cq_wait_init(&optee->call_queue, &waiter, system_thread);
	while (true) {
		if (resume.resume_token)
			ret = optee_rpmi_call_with_status(optee, &resume,
							  sizeof(resume), &resp,
							  sizeof(resp), &status);
		else
			ret = optee_rpmi_call_with_status(optee, req, sizeof(*req),
							  &resp, sizeof(resp),
							  &status);
		if (ret)
			goto done;

		switch (status) {
		case RPMI_SUCCESS:
			break;
		case RPMI_ERR_BUSY:
			if (!resume.resume_token) {
				optee_cq_wait_for_completion(&optee->call_queue,
							     &waiter);
				continue;
			}

			fallthrough;
		default:
			ret = rpmi_to_linux_error(status);
			goto done;
		}

		result = get_unaligned_le32(&resp.result);
		if (result == OPTEE_RPMI_YIELDING_CALL_RETURN_DONE)
			goto done;

		cond_resched();
		optee_rpmi_handle_rpc(ctx, optee, result, rpc_arg);

		resume.resume_token = resp.resume_token;
	}
done:
	optee_cq_wait_final(&optee->call_queue, &waiter);

	return ret;
}

/* The caller supplies SHM with room for command and RPC args. */
static int optee_rpmi_do_call_with_arg(struct tee_context *ctx,
				       struct tee_shm *shm, u_int offs,
				       bool system_thread)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct optee_msg_arg *arg, *rpc_arg;
	struct optee_rpmi_call_req req;
	size_t arg_size, rpc_size, rpc_offset;
	u32 parcel_id, nonce;

	arg = tee_shm_get_va(shm, offs);
	if (IS_ERR(arg))
		return PTR_ERR(arg);

	arg_size = OPTEE_MSG_GET_ARG_SIZE(arg->num_params);
	rpc_size = OPTEE_MSG_GET_ARG_SIZE(optee->rpc_param_count);
	rpc_offset = offs + arg_size;
	rpc_arg = tee_shm_get_va(shm, rpc_offset);
	if (IS_ERR(rpc_arg))
		return PTR_ERR(rpc_arg);

	optee_rpmi_shm_get_identity(shm, &parcel_id, &nonce);

	req.op = cpu_to_le32(OPTEE_RPMI_YIELDING_CALL_WITH_ARG);
	req.parcel_id = cpu_to_le32(parcel_id);
	req.nonce = cpu_to_le32(nonce);
	req.arg_offset = cpu_to_le64((u64)shm->offset + offs);
	req.rpc_offset = cpu_to_le64((u64)shm->offset + rpc_offset);
	req.arg_size = cpu_to_le32(arg_size);
	req.rpc_size = cpu_to_le32(rpc_size);

	return optee_rpmi_yielding_call(ctx, &req, rpc_arg, system_thread);
}

/* Yielding bottom halves run here, not in the transport retrieval worker. */
static void optee_rpmi_notif_work(struct work_struct *work)
{
	struct optee_rpmi *rpmi = container_of(work, struct optee_rpmi, notif_work);
	struct optee *optee = container_of(rpmi, struct optee, rpmi);

	optee_do_bottom_half(optee->ctx);
}

static void optee_rpmi_notif_callback(struct rpmi_tee_device *rdev, u32 signal,
				      void *cb_data)
{
	struct optee *optee = cb_data;

	queue_work(optee->rpmi.notif_wq, &optee->rpmi.notif_work);
}

/* Relinquish is not a barrier for an already-selected transport callback. */
static void optee_rpmi_async_notif_uninit(struct optee *optee)
{
	struct optee_rpmi *rpmi = &optee->rpmi;
	struct rpmi_tee_device *rdev = rpmi->rdev;
	int ret;

	if (!rpmi->notif_wq)
		return;

	ret = optee_stop_async_notif(optee->ctx);
	if (ret)
		dev_warn(&rdev->dev, "stop notifications failed: %d\n", ret);

	ret = rdev->ops->notifier_ops->notify_relinquish(rdev, rpmi->signal);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(&rdev->dev,
			 "relinquish notification failed: %d\n", ret);

	destroy_workqueue(rpmi->notif_wq);

	rpmi->notif_wq = NULL;
}

/* Enable OP-TEE's bottom-half doorbell using the reserved RPMI signal. */
static int optee_rpmi_enable_async_notif(struct optee *optee)
{
	struct optee_rpmi_enable_notif_req req = {
		.op = cpu_to_le32(OPTEE_RPMI_ENABLE_ASYNC_NOTIF),
		.signal_id = cpu_to_le32(optee->rpmi.signal),
	};
	struct optee_rpmi_status_resp resp;

	return optee_rpmi_call(optee, &req, sizeof(req), &resp, sizeof(resp));
}

static int optee_rpmi_async_notif_init(struct optee *optee)
{
	struct optee_rpmi *rpmi = &optee->rpmi;
	struct rpmi_tee_device *rdev = rpmi->rdev;
	int ret;

	INIT_WORK(&rpmi->notif_work, optee_rpmi_notif_work);
	rpmi->notif_wq = alloc_workqueue("optee_rpmi_notif", WQ_UNBOUND, 1);
	if (!rpmi->notif_wq)
		return -ENOMEM;

	ret = rdev->ops->notifier_ops->notify_request(rdev,
						      optee_rpmi_notif_callback,
						      optee, &rpmi->signal);
	if (ret) {
		destroy_workqueue(rpmi->notif_wq);
		/* Checked in optee_rpmi_async_notif_uninit(). */
		rpmi->notif_wq = NULL;

		return ret;
	}

	ret = optee_rpmi_enable_async_notif(optee);
	if (ret)
		optee_rpmi_async_notif_uninit(optee);

	return ret;
}

/* Query and store the trusted OS revision. */
static int optee_rpmi_get_os_version(struct optee *optee)
{
	struct optee_rpmi_probe_req req = {
		.op = cpu_to_le32(OPTEE_RPMI_GET_OS_VERSION),
	};
	struct optee_rpmi_os_resp os;
	int ret;

	ret = optee_rpmi_call(optee, &req, sizeof(req), &os, sizeof(os));
	if (ret)
		return ret;

	optee->revision.os_major = get_unaligned_le32(&os.major);
	optee->revision.os_minor = get_unaligned_le32(&os.minor);
	optee->revision.os_build_id = get_unaligned_le64(&os.build_id);

	if (optee->revision.os_build_id)
		pr_info("revision %u.%u (%016llx)\n",
			optee->revision.os_major, optee->revision.os_minor,
			optee->revision.os_build_id);
	else
		pr_info("revision %u.%u\n", optee->revision.os_major,
			optee->revision.os_minor);

	return 0;
}

/* Query and store secure-world capabilities and buffer limits. */
static int optee_rpmi_exchange_caps(struct optee *optee)
{
	struct optee_rpmi_probe_req req = {
		.op = cpu_to_le32(OPTEE_RPMI_EXCHANGE_CAPABILITIES),
	};
	struct optee_rpmi_caps_resp caps;
	u32 rpc_count, sec_caps, notif_count;
	int ret;

	ret = optee_rpmi_call(optee, &req, sizeof(req), &caps, sizeof(caps));
	if (ret)
		return ret;

	sec_caps = get_unaligned_le32(&caps.secure_caps);
	rpc_count = get_unaligned_le32(&caps.rpc_param_count);
	notif_count = get_unaligned_le32(&caps.notification_count);
	if (!notif_count || !rpc_count)
		return -EPROTO;

	optee->rpc_param_count = rpc_count;
	optee->rpmi.sec_caps = sec_caps;
	optee->rpmi.notification_count = notif_count;
	optee->in_kernel_rpmb_routing = IS_REACHABLE(CONFIG_RPMB);

	return 0;
}

static int optee_rpmi_api_is_compatible(struct optee *optee)
{
	struct optee_rpmi_probe_req req = {
		.op = cpu_to_le32(OPTEE_RPMI_GET_API_VERSION),
	};
	struct optee_rpmi_api_resp api;
	int ret;

	ret = optee_rpmi_call(optee, &req, sizeof(req), &api, sizeof(api));
	if (ret)
		return ret;

	if (get_unaligned_le32(&api.major) != OPTEE_RPMI_VERSION_MAJOR)
		return -EPROTONOSUPPORT;

	/* Version 1.0 has no minimum minor revision beyond zero. */
	return 0;
}

static void optee_rpmi_get_version(struct tee_device *teedev,
				   struct tee_ioctl_version_data *vers)
{
	*vers = (struct tee_ioctl_version_data) {
		.impl_id = TEE_IMPL_ID_OPTEE,
		.gen_caps = TEE_GEN_CAP_GP | TEE_GEN_CAP_REG_MEM |
			    TEE_GEN_CAP_MEMREF_NULL,
	};
}

static int optee_rpmi_open(struct tee_context *ctx)
{
	return optee_open(ctx, true);
}

static const struct tee_driver_ops optee_rpmi_clnt_ops = {
	.get_version = optee_rpmi_get_version,
	.get_tee_revision = optee_get_revision,
	.open = optee_rpmi_open,
	.release = optee_release,
	.open_session = optee_open_session,
	.close_session = optee_close_session,
	.invoke_func = optee_invoke_func,
	.cancel_req = optee_cancel_req,
	.shm_register = optee_rpmi_shm_register,
	.shm_unregister = optee_rpmi_shm_unregister,
};

static const struct tee_driver_ops optee_rpmi_supp_ops = {
	.get_version = optee_rpmi_get_version,
	.get_tee_revision = optee_get_revision,
	.open = optee_rpmi_open,
	.release = optee_release_supp,
	.supp_recv = optee_supp_recv,
	.supp_send = optee_supp_send,
	.shm_register = optee_rpmi_shm_register,
	.shm_unregister = optee_rpmi_shm_unregister_supp,
};

static const struct tee_desc optee_rpmi_clnt_desc = {
	.name = DRIVER_NAME "-rpmi-clnt",
	.ops = &optee_rpmi_clnt_ops,
	.owner = THIS_MODULE,
};

static const struct tee_desc optee_rpmi_supp_desc = {
	.name = DRIVER_NAME "-rpmi-supp",
	.ops = &optee_rpmi_supp_ops,
	.owner = THIS_MODULE,
	.flags = TEE_DESC_PRIVILEGED,
};

static const struct optee_ops optee_rpmi_ops = {
	.do_call_with_arg = optee_rpmi_do_call_with_arg,
	.to_msg_param = optee_rpmi_to_msg_param,
	.from_msg_param = optee_rpmi_from_msg_param,
};

/* Keep callback state and memory tables alive until all TEE users release. */
static void optee_rpmi_remove(struct rpmi_tee_device *rdev)
{
	struct optee *optee = dev_get_drvdata(&rdev->dev);

	optee_rpmi_async_notif_uninit(optee);
	optee_remove_common(optee);
	optee_rpmi_shm_rht_uninit(optee);
	kfree(optee);
}

static int optee_rpmi_probe(struct rpmi_tee_device *rdev)
{
	struct tee_device *teedev;
	struct tee_context *ctx;
	int ret;

	struct optee *optee __free(kfree) = kzalloc_obj(*optee);
	if (!optee)
		return -ENOMEM;

	optee->rpmi.rdev = rdev;
	optee->ops = &optee_rpmi_ops;

	ret = optee_rpmi_api_is_compatible(optee);
	if (ret)
		return ret;

	ret = optee_rpmi_get_os_version(optee);
	if (ret)
		return ret;

	ret = optee_rpmi_exchange_caps(optee);
	if (ret)
		return ret;

	optee->pool = optee_rpmi_shm_pool_alloc();
	if (IS_ERR(optee->pool))
		return PTR_ERR(optee->pool);

	ret = optee_rpmi_shm_rht_init(optee);
	if (ret)
		goto err_pool;

	optee_cq_init(&optee->call_queue, 0);
	optee_supp_init(&optee->supp);
	optee_shm_arg_cache_init(optee, OPTEE_SHM_ARG_SHARED);
	mutex_init(&optee->rpmb_dev_mutex);
	INIT_WORK(&optee->rpmb_scan_bus_work, optee_bus_scan_rpmb);
	optee->rpmb_intf.notifier_call = optee_rpmb_intf_rdev;
	ret = optee_notif_init(optee, optee->rpmi.notification_count);
	if (ret)
		goto err_common;

	/* Allocate all keys, then restrict the inclusive bound to the last key. */
	optee->notif.max_key = optee->rpmi.notification_count - 1;

	teedev = tee_device_alloc(&optee_rpmi_clnt_desc, &rdev->dev,
				  optee->pool, optee);
	if (IS_ERR(teedev)) {
		ret = PTR_ERR(teedev);
		goto err_notif;
	}
	optee->teedev = teedev;

	teedev = tee_device_alloc(&optee_rpmi_supp_desc, &rdev->dev,
				  optee->pool, optee);
	if (IS_ERR(teedev)) {
		ret = PTR_ERR(teedev);
		goto err_devices;
	}
	optee->supp_teedev = teedev;

	optee_set_dev_group(optee);

	/* Internal RPC allocation must be ready before userspace can enter. */
	ctx = teedev_open(optee->teedev);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto err_devices;
	}

	optee->ctx = ctx;
	dev_set_drvdata(&rdev->dev, optee);
	ret = optee_rpmi_async_notif_init(optee);
	if (ret)
		dev_warn(&rdev->dev, "async notifications unavailable: %d\n", ret);

	if (optee->in_kernel_rpmb_routing)
		blocking_notifier_chain_register(&optee_rpmb_intf_added,
						 &optee->rpmb_intf);

	ret = tee_device_register(optee->teedev);
	if (ret)
		goto err_initialized;

	ret = tee_device_register(optee->supp_teedev);
	if (ret)
		goto err_initialized;

	ret = optee_enumerate_devices(PTA_CMD_GET_DEVICES);
	if (ret)
		goto err_initialized;

	dev_info(&rdev->dev, "OP-TEE RPMI %u.%u initialized\n",
		 optee->revision.os_major, optee->revision.os_minor);
	retain_and_null_ptr(optee);

	return 0;

err_initialized:
	/* The remove path owns and frees the published backend state. */
	retain_and_null_ptr(optee);
	optee_rpmi_remove(rdev);

	return ret;
err_devices:
	tee_device_unregister(optee->supp_teedev);
	tee_device_unregister(optee->teedev);
	optee_shm_arg_cache_uninit(optee);
err_notif:
	optee_notif_uninit(optee);
err_common:
	optee_supp_uninit(&optee->supp);
	mutex_destroy(&optee->call_queue.mutex);
	rpmb_dev_put(optee->rpmb_dev);
	mutex_destroy(&optee->rpmb_dev_mutex);
	optee_rpmi_shm_rht_uninit(optee);
err_pool:
	tee_shm_pool_free(optee->pool);

	return ret;
}

static const struct rpmi_tee_device_id optee_rpmi_device_ids[] = {
	{ OPTEE_RPMI_SERVICE_UUID },
	{}
};

static struct rpmi_tee_driver optee_rpmi_driver = {
	.name = DRIVER_NAME "-rpmi",
	.probe = optee_rpmi_probe,
	.remove = optee_rpmi_remove,
	.id_table = optee_rpmi_device_ids,
};

int optee_rpmi_abi_register(void)
{
	return rpmi_tee_register(&optee_rpmi_driver);
}

void optee_rpmi_abi_unregister(void)
{
	rpmi_tee_unregister(&optee_rpmi_driver);
}

MODULE_ALIAS("rpmi_tee:486178e0-e7f8-11e3-bc5e-0002a5d5c51b");
