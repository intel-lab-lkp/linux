// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 * REE->TEE session management and yield-call interface.
 */
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/device.h>
#include "mbedtee_drv.h"

#define MBEDTEE_RPC_CALL_ACTIVE			0
#define MBEDTEE_RPC_CALL_DONE			1
#define MBEDTEE_RPC_CALL_INTERRUPTED	2

int mbedtee_rpc_call_alloc(struct mbedtee_device *mbedtee,
			   size_t payload_size, struct mbedtee_rpc_call **call)
{
	struct mbedtee_rpc_call *rcall;
	unsigned long rpc_index;
	int ret;

	if (sizeof(*rcall) + payload_size > PAGE_SIZE)
		return -E2BIG;

	rcall = kzalloc(sizeof(*rcall) + payload_size, GFP_KERNEL);
	if (!rcall)
		return -ENOMEM;

	init_completion(&rcall->c);
	rcall->state = MBEDTEE_RPC_CALL_ACTIVE;
	rcall->rpc.size = payload_size;
	rcall->rpc.ret = -EOPNOTSUPP;
	rcall->rpc_phys = mbedtee_virt_to_phys(&rcall->rpc);

	do {
		rpc_index = atomic_long_inc_return(&mbedtee->rpc_call_seq);
	} while (rpc_index == 0);

	ret = xa_insert_irq(&mbedtee->rpc_calls, rpc_index, rcall, GFP_KERNEL);
	if (ret != 0) {
		kfree(rcall);
		return ret;
	}

	rcall->rpc.waiter_id = rpc_index;
	*call = rcall;

	return 0;
}

void mbedtee_rpc_call_free(struct mbedtee_device *mbedtee,
			   struct mbedtee_rpc_call *call)
{
	unsigned long rpc_index;

	if (!call)
		return;

	rpc_index = (unsigned long)call->rpc.waiter_id;

	/*
	 * If interrupted/completed, complete_call owns this allocation
	 * and will xa_erase + kfree when the TEE eventually finishes.
	 */
	if (READ_ONCE(call->state) == MBEDTEE_RPC_CALL_INTERRUPTED)
		return;

	xa_erase_irq(&mbedtee->rpc_calls, rpc_index);
	kfree(call);
}

void mbedtee_rpc_complete_call(struct mbedtee_device *mbedtee,
			       u64 waiter_id)
{
	struct mbedtee_rpc_call *call;
	unsigned long rpc_index;
	unsigned long flags;
	unsigned int state;

	rpc_index = (unsigned long)waiter_id;
	if ((u64)rpc_index != waiter_id)
		return;

	xa_lock_irqsave(&mbedtee->rpc_calls, flags);
	call = xa_load(&mbedtee->rpc_calls, rpc_index);
	if (!call) {
		xa_unlock_irqrestore(&mbedtee->rpc_calls, flags);
		return;
	}

	state = cmpxchg(&call->state, MBEDTEE_RPC_CALL_ACTIVE,
			MBEDTEE_RPC_CALL_DONE);
	__xa_erase(&mbedtee->rpc_calls, rpc_index);

	if (state == MBEDTEE_RPC_CALL_INTERRUPTED) {
		xa_unlock_irqrestore(&mbedtee->rpc_calls, flags);
		kfree(call);
		return;
	}

	complete(&call->c);
	xa_unlock_irqrestore(&mbedtee->rpc_calls, flags);
}

long mbedtee_rpc_wait_for_completion(struct mbedtee_device *mbedtee,
				     struct mbedtee_rpc_call *call, bool killable)
{
	long ret;
	unsigned int state;
	unsigned long flags;
	unsigned long rpc_index;

	if (!killable) {
		wait_for_completion(&call->c);
		return 0;
	}

	rpc_index = (unsigned long)call->rpc.waiter_id;

	ret = wait_for_completion_killable(&call->c);
	if (ret == 0)
		return 0;

	/*
	 * Synchronize with mbedtee_rpc_complete_call() via the
	 * xa_lock. If complete_call already consumed this entry
	 * before we acquired the lock, the entry is already gone
	 * from the xarray and the TEE already completed (or freed)
	 * the call -- treat as success since the RPC is finished.
	 * If the entry is still present, we hold the lock so
	 * complete_call cannot race with our state transition.
	 */
	xa_lock_irqsave(&mbedtee->rpc_calls, flags);
	if (xa_load(&mbedtee->rpc_calls, rpc_index) != call) {
		xa_unlock_irqrestore(&mbedtee->rpc_calls, flags);
		return 0;
	}

	state = cmpxchg(&call->state, MBEDTEE_RPC_CALL_ACTIVE,
			MBEDTEE_RPC_CALL_INTERRUPTED);
	if (state == MBEDTEE_RPC_CALL_DONE) {
		xa_unlock_irqrestore(&mbedtee->rpc_calls, flags);
		return 0;
	}

	if (state == MBEDTEE_RPC_CALL_ACTIVE)
		/* Publish interruption state before the TEE waiter reads it. */
		smp_store_release(&call->rpc.interrupted, true);

	xa_unlock_irqrestore(&mbedtee->rpc_calls, flags);
	return ret;
}

/* Local helpers for encoding/decoding the 4-bit-per-parameter type field. */
static inline u32 mbedtee_param_type_get(u32 types, unsigned int idx)
{
	return (types >> (idx * 4)) & 0xF;
}

static inline u32 mbedtee_param_type_set(u32 type, unsigned int idx)
{
	return (type & 0xF) << (idx * 4);
}

static int mbedtee_param_decode(struct tee_param *params,
				size_t num_params, const struct rpc_param *rp)
{
	size_t n;

	if (num_params > ARRAY_SIZE(rp->params))
		return -EINVAL;

	for (n = 0; n < num_params; n++) {
		struct tee_param *p = params + n;
		const union rpc_tee_param *rtp = rp->params + n;
		u32 attr = mbedtee_param_type_get(rp->params_type, n);

		switch (attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_NONE:
			memset(&p->u, 0, sizeof(p->u));
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT:
			p->u.value.a = rtp->value.a;
			p->u.value.b = rtp->value.b;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
			p->u.memref.size = rtp->memref.size;
			break;
		default:
			return -EINVAL;
		}
		p->attr = attr;
	}
	return 0;
}

static int mbedtee_param_encode(struct rpc_param *rp,
				size_t num_params, const struct tee_param *params)
{
	size_t n;

	if (num_params > ARRAY_SIZE(rp->params))
		return -EINVAL;

	rp->params_type = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;

	for (n = 0; n < num_params; n++) {
		const struct tee_param *p = params + n;
		union rpc_tee_param *rtp = rp->params + n;

		rp->params_type |= mbedtee_param_type_set(p->attr, n);

		switch (p->attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_NONE:
			memset(rtp, 0, sizeof(*rtp));
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT:
			rtp->value.a = p->u.value.a;
			rtp->value.b = p->u.value.b;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
			if (p->u.memref.shm)
				rtp->memref.id = p->u.memref.shm->sec_world_id;
			else
				rtp->memref.id = 0; /* invalid-id @ mbedtee */
			rtp->memref.size = p->u.memref.size;
			rtp->memref.offset = p->u.memref.shm_offs;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * cancel_req() and shm_{register,unregister} still return Linux errno to the
 * kernel TEE core. Preserve any local transport failure in @ret; otherwise
 * fold a non-success TEE-side GP status into the generic -EIO expected by
 * these internal-only hooks.
 */
static int mbedtee_rpc_gp_ret_to_errno(int ret, s32 gp_ret)
{
	if (ret)
		return ret;

	return gp_ret == TEEC_SUCCESS ? 0 : -EIO;
}

static struct mbedtee_session *
mbedtee_find_session_locked(struct mbedtee_context_data *ctxdata,
			    u32 session_id)
{
	struct mbedtee_session *sess;

	list_for_each_entry(sess, &ctxdata->sess_list, list_node)
		if (sess->session_id == session_id)
			return sess;

	return NULL;
}

int mbedtee_open_session(struct tee_context *ctx,
			 struct tee_ioctl_open_session_arg *arg,
			 struct tee_param *param)
{
	int ret;
	u32 session_id = 0;
	struct mbedtee_context_data *ctxdata = ctx->data;
	struct mbedtee_device *mbedtee = tee_get_drvdata(ctx->teedev);
	struct mbedtee_rpc_call *call;
	struct rpc_param *rp;
	struct mbedtee_session *sess;

	sess = kzalloc_obj(*sess, GFP_KERNEL);
	if (!sess)
		return -ENOMEM;

	ret = mbedtee_rpc_call_alloc(mbedtee, sizeof(*rp), &call);
	if (ret != 0) {
		kfree(sess);
		return ret;
	}

	rp = (struct rpc_param *)call->rpc.data;

	ret = mbedtee_param_encode(rp, arg->num_params, param);
	if (ret) {
		kfree(sess);
		mbedtee_rpc_call_free(mbedtee, call);
		return ret;
	}

	memcpy(rp->uuid, arg->uuid, sizeof(arg->uuid));
	memcpy(rp->clnt_uuid, arg->clnt_uuid, sizeof(arg->clnt_uuid));
	rp->ret_origin = TEEC_ORIGIN_COMMS;

	ret = mbedtee_rpc_yieldcall(mbedtee, MBEDTEE_RPC_OPEN_SESSION, call, false);

	dev_dbg(mbedtee->dev, "open session ret %d gp_ret %d\n", ret,
		call->rpc.ret);

	if (ret == 0 && call->rpc.ret == TEEC_SUCCESS) {
		session_id = rp->session_id;
		sess->session_id = session_id;
		mutex_lock(&ctxdata->mutex);
		list_add(&sess->list_node, &ctxdata->sess_list);
		mutex_unlock(&ctxdata->mutex);
	} else {
		kfree(sess);
	}

	if (ret != 0) {
		arg->ret = TEEC_ERROR_COMMUNICATION;
		arg->ret_origin = TEEC_ORIGIN_COMMS;
	} else if (mbedtee_param_decode(param, arg->num_params, rp)) {
		arg->ret = TEEC_ERROR_COMMUNICATION;
		arg->ret_origin = TEEC_ORIGIN_COMMS;
		if (call->rpc.ret == TEEC_SUCCESS)
			mbedtee_close_session(ctx, session_id);
	} else {
		arg->session = rp->session_id;
		arg->ret = (u32)call->rpc.ret;
		arg->ret_origin = rp->ret_origin;
	}

	mbedtee_rpc_call_free(mbedtee, call);
	return 0;
}

int mbedtee_invoke_func(struct tee_context *ctx,
			struct tee_ioctl_invoke_arg *arg,
			struct tee_param *param)
{
	int ret;
	struct mbedtee_context_data *ctxdata = ctx->data;
	struct mbedtee_device *mbedtee = tee_get_drvdata(ctx->teedev);
	struct mbedtee_rpc_call *call;
	struct rpc_param *rp;
	struct mbedtee_session *sess;

	mutex_lock(&ctxdata->mutex);
	sess = mbedtee_find_session_locked(ctxdata, arg->session);
	mutex_unlock(&ctxdata->mutex);
	if (!sess)
		return -EINVAL;

	ret = mbedtee_rpc_call_alloc(mbedtee, sizeof(*rp), &call);
	if (ret != 0)
		return ret;

	rp = (struct rpc_param *)call->rpc.data;

	rp->session_id = arg->session;
	rp->cmd_id = arg->func;
	rp->ret_origin = TEEC_ORIGIN_COMMS;

	ret = mbedtee_param_encode(rp, arg->num_params, param);
	if (ret) {
		mbedtee_rpc_call_free(mbedtee, call);
		return ret;
	}

	ret = mbedtee_rpc_yieldcall(mbedtee, MBEDTEE_RPC_INVOKE_SESSION, call, true);

	dev_dbg(mbedtee->dev, "invoke session ret %d gp_ret %d\n", ret,
		call->rpc.ret);

	if (ret != 0) {
		arg->ret = TEEC_ERROR_COMMUNICATION;
		arg->ret_origin = TEEC_ORIGIN_COMMS;
	} else if (mbedtee_param_decode(param, arg->num_params, rp)) {
		arg->ret = TEEC_ERROR_COMMUNICATION;
		arg->ret_origin = TEEC_ORIGIN_COMMS;
	} else {
		arg->ret = (u32)call->rpc.ret;
		arg->ret_origin = rp->ret_origin;
	}

	mbedtee_rpc_call_free(mbedtee, call);
	return 0;
}

int mbedtee_close_session(struct tee_context *ctx, u32 session)
{
	int ret;
	struct mbedtee_context_data *ctxdata = ctx->data;
	struct mbedtee_device *mbedtee = tee_get_drvdata(ctx->teedev);
	struct mbedtee_rpc_call *call;
	struct rpc_param *rp;
	struct mbedtee_session *sess;

	mutex_lock(&ctxdata->mutex);
	sess = mbedtee_find_session_locked(ctxdata, session);
	if (!sess) {
		mutex_unlock(&ctxdata->mutex);
		return -EINVAL;
	}

	ret = mbedtee_rpc_call_alloc(mbedtee, sizeof(*rp), &call);
	if (ret != 0) {
		/*
		 * RPC allocation failed but the session is still alive on
		 * the TEE side. Leave it in the session list so a future
		 * close attempt can retry; warn so the condition is visible.
		 */
		mutex_unlock(&ctxdata->mutex);
		return ret;
	}

	list_del(&sess->list_node);
	mutex_unlock(&ctxdata->mutex);

	rp = (struct rpc_param *)call->rpc.data;
	rp->session_id = session;

	ret = mbedtee_rpc_yieldcall(mbedtee, MBEDTEE_RPC_CLOSE_SESSION, call, false);

	mbedtee_rpc_call_free(mbedtee, call);
	kfree(sess);

	return ret;
}

int mbedtee_cancel_req(struct tee_context *ctx,
		       u32 cancel_id, u32 session)
{
	int ret;
	struct mbedtee_context_data *ctxdata = ctx->data;
	struct mbedtee_device *mbedtee = tee_get_drvdata(ctx->teedev);
	struct mbedtee_rpc_call *call;
	struct rpc_cancel_req *cancel;
	struct mbedtee_session *sess;

	mutex_lock(&ctxdata->mutex);
	sess = mbedtee_find_session_locked(ctxdata, session);
	mutex_unlock(&ctxdata->mutex);
	if (!sess)
		return -EINVAL;

	ret = mbedtee_rpc_call_alloc(mbedtee, sizeof(*cancel), &call);
	if (ret != 0)
		return ret;

	cancel = (struct rpc_cancel_req *)call->rpc.data;
	cancel->session_id = session;
	cancel->cancel_id = cancel_id;

	ret = mbedtee_rpc_yieldcall(mbedtee, MBEDTEE_RPC_CANCEL, call, false);
	ret = mbedtee_rpc_gp_ret_to_errno(ret, call->rpc.ret);

	mbedtee_rpc_call_free(mbedtee, call);

	return ret;
}

int mbedtee_shm_register(struct tee_context *ctx, struct tee_shm *shm,
			 struct page **pages, size_t nr_pages, unsigned long start)
{
	int ret;
	size_t i, j;
	u64 *pagearray;
	struct mbedtee_device *mbedtee = tee_get_drvdata(ctx->teedev);
	struct mbedtee_rpc_call *call;
	struct rpc_memref *memref;
	/* Multiple: number of MBEDTEE_PAGE_SIZE pages per Linux page. */
	const size_t multiple = PAGE_SIZE / MBEDTEE_PAGE_SIZE;

	BUILD_BUG_ON(PAGE_SIZE < MBEDTEE_PAGE_SIZE);

	ret = mbedtee_rpc_call_alloc(mbedtee, sizeof(*memref), &call);
	if (ret != 0)
		return ret;

	memref = (struct rpc_memref *)call->rpc.data;

	pagearray = kcalloc(nr_pages * multiple, sizeof(*pagearray), GFP_KERNEL);
	if (!pagearray) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < nr_pages; i++) {
		for (j = 0; j < multiple; j++)
			pagearray[i * multiple + j] = MBEDTEE_PAGE_SIZE * j +
				page_to_phys(pages[i]);
	}

	memref->size = tee_shm_get_size(shm);
	memref->offset = tee_shm_get_page_offset(shm);
	memref->pages = mbedtee_virt_to_phys(pagearray);
	memref->cnt = nr_pages * multiple;
	ret = mbedtee_rpc_yieldcall(mbedtee, MBEDTEE_RPC_REGISTER_SHM, call, false);
	ret = mbedtee_rpc_gp_ret_to_errno(ret, call->rpc.ret);

	if (ret == 0)
		shm->sec_world_id = memref->id;

	kfree(pagearray);

out:
	mbedtee_rpc_call_free(mbedtee, call);
	return ret;
}

int mbedtee_shm_unregister(struct tee_context *ctx,
			   struct tee_shm *shm)
{
	int ret;
	struct mbedtee_device *mbedtee = tee_get_drvdata(ctx->teedev);
	struct mbedtee_rpc_call *call;
	struct rpc_memref *memref;

	ret = mbedtee_rpc_call_alloc(mbedtee, sizeof(*memref), &call);
	if (ret != 0)
		return ret;

	memref = (struct rpc_memref *)call->rpc.data;
	memref->id = shm->sec_world_id;

	ret = mbedtee_rpc_yieldcall(mbedtee, MBEDTEE_RPC_UNREGISTER_SHM, call, false);
	ret = mbedtee_rpc_gp_ret_to_errno(ret, call->rpc.ret);

	mbedtee_rpc_call_free(mbedtee, call);

	return ret;
}
