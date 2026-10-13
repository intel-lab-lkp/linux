// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/overflow.h>
#include <linux/rpmi_tee.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include "optee_private.h"
#include "optee_rpmi.h"

/* Keep transport errors separate from the control status in a response. */
static int optee_rpmi_call_with_status(struct optee *optee,
				       const void *req, size_t req_len,
				       void *resp, size_t resp_size,
				       u32 *status)
{
	struct rpmi_tee_device *rdev = optee->rpmi.rdev;
	size_t received = resp_size;
	int rc;

	rc = rdev->ops->msg_ops->call(rdev, req, req_len, resp, &received);
	if (rc)
		return rc;

	if (received != resp_size)
		return -EPROTO;

	*status = get_unaligned_le32(resp);

	return 0;
}

/**
 * optee_rpmi_call - Send a control request and check its TEE result
 * @optee: OP-TEE instance.
 * @req: Control request, including the operation number.
 * @req_len: Request size in bytes.
 * @resp: Response buffer, beginning with a little-endian TEE result.
 * @resp_size: Exact expected response size, including the status field.
 *
 * Return: 0 on success, a transport error, -EPROTO for an unexpected response
 * size, or -EIO for an unsuccessful control response.
 */
static int optee_rpmi_call(struct optee *optee, const void *req, size_t req_len,
			   void *resp, size_t resp_size)
{
	u32 status;
	int rc;

	rc = optee_rpmi_call_with_status(optee, req, req_len, resp, resp_size,
					 &status);
	if (rc)
		return rc;
	else if (status != TEEC_SUCCESS)
		return -EIO;

	return 0;
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
		.receivers = &receiver,
		.receiver_count = 1,
		.creator_access = RPMI_TEE_MEM_ACCESS_READ |
				  RPMI_TEE_MEM_ACCESS_WRITE,
	};
	struct sg_table sgt;
	u64 global_id;
	int rc;

	rc = optee_check_mem_type(start, num_pages);
	if (rc)
		return rc;

	rc = sg_alloc_table_from_pages(&sgt, pages, num_pages, 0,
				       num_pages * PAGE_SIZE, GFP_KERNEL);
	if (rc)
		return rc;

	rc = ida_alloc_min(&optee->rpmi.nonce_ida, 1, GFP_KERNEL);
	if (rc < 0) {
		sg_free_table(&sgt);
		return rc;
	}

	args.nonce = rc;
	args.sg = sgt.sgl;
	rc = rdev->ops->mem_ops->memory_share(rdev, &args);
	sg_free_table(&sgt);
	/* A failed exchange may leave a parcel using this nonce. */
	if (rc)
		return rc;

	global_id = optee_rpmi_global_id(args.parcel_id, args.nonce);

	rc = optee_shm_handle_map_add(&optee->rpmi.shm_map, shm, global_id);
	if (rc) {
		int rrc;

		rrc = rdev->ops->mem_ops->memory_reclaim(rdev, args.parcel_id);
		/* Keep the nonce if firmware may still hold the parcel. */
		if (rrc)
			dev_err(&rdev->dev, "reclaim parcel %#x failed: %d\n",
				args.parcel_id, rrc);
		else
			ida_free(&optee->rpmi.nonce_ida, args.nonce);

		return rc;
	}

	shm->sec_world_id = global_id;

	return 0;
}

static int optee_rpmi_shm_unregister(struct tee_context *ctx,
				     struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct rpmi_tee_device *rdev = optee->rpmi.rdev;
	struct optee_rpmi_unregister_req req = {
		.op = cpu_to_le32(OPTEE_RPMI_UNREGISTER_SHM),
		.global_id = cpu_to_le64(shm->sec_world_id),
	};
	struct optee_rpmi_status_resp resp;
	u32 parcel_id = optee_rpmi_parcel_id(shm->sec_world_id);
	u32 nonce = optee_rpmi_nonce(shm->sec_world_id);
	int rc;

	optee_shm_handle_map_remove(&optee->rpmi.shm_map, shm->sec_world_id);
	shm->sec_world_id = 0;

	rc = optee_rpmi_call(optee, &req, sizeof(req), &resp, sizeof(resp));
	if (rc)
		dev_err(&rdev->dev, "unregister parcel %#x failed: %d\n",
			parcel_id, rc);

	rc = rdev->ops->mem_ops->memory_reclaim(rdev, parcel_id);
	/* Keep the nonce if firmware may still hold the parcel. */
	if (rc)
		dev_err(&rdev->dev, "reclaim parcel %#x failed: %d\n",
			parcel_id, rc);
	else
		ida_free(&optee->rpmi.nonce_ida, nonce);

	return rc;
}

static int optee_rpmi_shm_unregister_supp(struct tee_context *ctx,
					  struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct rpmi_tee_device *rdev = optee->rpmi.rdev;
	u32 parcel_id = optee_rpmi_parcel_id(shm->sec_world_id);
	u32 nonce = optee_rpmi_nonce(shm->sec_world_id);
	int rc;

	optee_shm_handle_map_remove(&optee->rpmi.shm_map, shm->sec_world_id);
	shm->sec_world_id = 0;
	/* OP-TEE has already retired the parcel through SHM_FREE RPC. */
	rc = rdev->ops->mem_ops->memory_reclaim(rdev, parcel_id);
	/* Keep the nonce if firmware may still hold the parcel. */
	if (rc)
		dev_err(&rdev->dev, "reclaim parcel %#x failed: %d\n",
			parcel_id, rc);
	else
		ida_free(&optee->rpmi.nonce_ida, nonce);

	return rc;
}

/* Convert a memory reference to an OP-TEE RPMI parcel reference. */
static int to_msg_param_rpmi_mem(struct optee_msg_param *mp,
				 const struct tee_param *p)
{
	struct tee_shm *shm = p->u.memref.shm;

	mp->attr = OPTEE_MSG_ATTR_TYPE_PMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	memset(&mp->u, 0, sizeof(mp->u));
	/* For !shm, a zero global_id encodes a NULL reference. */
	if (shm) {
		if (check_add_overflow((u64)shm->offset,
				       (u64)p->u.memref.shm_offs,
				       &mp->u.pmem.offs))
			return -EINVAL;

		mp->u.pmem.global_id = shm->sec_world_id;
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

	if (!optee_rpmi_nonce(mp->u.pmem.global_id)) {
		/* Return NULL shm. */
		if (mp->u.pmem.offs)
			return -EINVAL;
		p->u.memref.shm = NULL;
		p->u.memref.shm_offs = 0;
		return 0;
	}

	shm = optee_shm_handle_map_lookup(&optee->rpmi.shm_map,
					  mp->u.pmem.global_id);
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
		int rc;

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
			rc = from_msg_param_rpmi_mem(optee, p, attr, mp);
			if (rc)
				return rc;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}
