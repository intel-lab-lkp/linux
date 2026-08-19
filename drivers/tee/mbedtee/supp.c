// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Linaro Limited
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 */
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "mbedtee_drv.h"

struct mbedtee_supp_req {
	u32 func;

	struct list_head node;
	struct task_struct *worker;
	struct tee_context *ctx;	/* owning supplicant context */

	struct tee_shm *shm;
	void *data;
	size_t size;

	struct completion c;
};

static int mbedtee_supp_check_recv_params(size_t num_params,
					  struct tee_param *params)
{
	if (num_params != 1)
		return -EINVAL;

	if (!tee_param_is_memref(params) || !params->u.memref.shm)
		return -EINVAL;

	return 0;
}

void mbedtee_supp_init(struct mbedtee_supp *supp)
{
	memset(supp, 0, sizeof(*supp));
	mutex_init(&supp->mutex);
	init_completion(&supp->reqs_c);
	INIT_LIST_HEAD(&supp->reqs);
	INIT_LIST_HEAD(&supp->active_reqs);
}

void mbedtee_supp_uninit(struct mbedtee_supp *supp)
{
	mutex_destroy(&supp->mutex);
}

static void mbedtee_supp_complete_req(struct mbedtee_supp_req *r)
{
	list_del_init(&r->node);
	if (r->shm)
		tee_shm_put(r->shm);
	r->shm = NULL;
	/* Tell the TEE that the request failed, not stale success data. */
	if (r->data && r->size >= sizeof(struct supp_cmd_hdr))
		((struct supp_cmd_hdr *)r->data)->ret = TEEC_ERROR_COMMUNICATION;
	complete(&r->c);
}

void mbedtee_supp_release(struct mbedtee_supp *supp, struct tee_context *ctx)
{
	struct mbedtee_supp_req *r, *n;

	mutex_lock(&supp->mutex);

	/*
	 * Cancel only requests owned by @ctx.  Unclaimed requests and
	 * requests owned by other supplicant contexts are left alone.
	 */
	list_for_each_entry_safe(r, n, &supp->reqs, node) {
		if (r->ctx != ctx)
			continue;
		mbedtee_supp_complete_req(r);
	}

	list_for_each_entry_safe(r, n, &supp->active_reqs, node) {
		if (r->ctx != ctx)
			continue;
		mbedtee_supp_complete_req(r);
	}

	if (supp->ctx == ctx)
		supp->ctx = NULL;

	mutex_unlock(&supp->mutex);
}

void mbedtee_supp_abort_all(struct mbedtee_supp *supp)
{
	struct mbedtee_supp_req *r, *n;

	mutex_lock(&supp->mutex);
	supp->shutting_down = true;

	list_for_each_entry_safe(r, n, &supp->reqs, node)
		mbedtee_supp_complete_req(r);

	list_for_each_entry_safe(r, n, &supp->active_reqs, node)
		mbedtee_supp_complete_req(r);

	supp->ctx = NULL;

	mutex_unlock(&supp->mutex);
	complete_all(&supp->reqs_c);
}

static int supp_enqueue_req(struct mbedtee_supp *supp,
			    struct mbedtee_supp_req *req)
{
	int ret = 0;

	mutex_lock(&supp->mutex);
	if (supp->shutting_down)
		ret = -ESHUTDOWN;
	else
		list_add_tail(&req->node, &supp->reqs);
	mutex_unlock(&supp->mutex);

	return ret;
}

/*
 * Receive an RPC request from TEE and dispatch it to supplicant.
 * Blocks until the supplicant sends back the result.
 */
void mbedtee_supp_handler(struct mbedtee_device *mbedtee,
			  u32 func, void *data, size_t size)
{
	struct mbedtee_supp *supp = &mbedtee->supp;
	struct mbedtee_supp_req *req = kzalloc_obj(*req, GFP_KERNEL);
	struct supp_cmd_hdr *cmd = data;
	int ret;

	if (!req) {
		if (cmd && size >= sizeof(*cmd))
			cmd->ret = TEEC_ERROR_OUT_OF_MEMORY;
		return;
	}

	init_completion(&req->c);
	req->func = func;
	req->data = data;
	req->size = size;

	ret = supp_enqueue_req(supp, req);
	if (ret != 0) {
		if (cmd && size >= sizeof(*cmd))
			cmd->ret = TEEC_ERROR_COMMUNICATION;
		kfree(req);
		return;
	}

	/* Wake up the supplicant daemon to handle this request. */
	complete(&supp->reqs_c);

	/*
	 * Wait for the supplicant to process and return the result, like
	 * OP-TEE does. If the supplicant dies, mbedtee_supp_release()
	 * completes the request, so this cannot block forever.
	 */
	wait_for_completion(&req->c);

	kfree(req);
}

/*
 * Called by supplicant to receive the next pending request.
 */
int mbedtee_supp_recv(struct tee_context *ctx,
		      u32 *func, u32 *num_params, struct tee_param *param)
{
	struct tee_device *teedev = ctx->teedev;
	struct mbedtee_device *mbedtee = tee_get_drvdata(teedev);
	struct mbedtee_supp *supp = &mbedtee->supp;
	struct mbedtee_supp_req *req;
	struct mbedtee_context_data *d = ctx->data;
	struct tee_shm *shm;
	int ret;

	ret = mbedtee_supp_check_recv_params(*num_params, param);
	if (ret != 0)
		return ret;

	shm = param->u.memref.shm;
	shm = tee_shm_get_from_id(ctx, shm->id);
	tee_shm_put(param->u.memref.shm);
	if (IS_ERR(shm)) {
		param->u.memref.shm = NULL;
		return PTR_ERR(shm);
	}
	param->u.memref.shm = shm;

	d->is_supp_ctx = true;

	/*
	 * Pop, own and validate the request in one critical section so that
	 * a concurrent supp_abort_all() can never free the request while
	 * it is being prepared here.
	 */
	while (true) {
		mutex_lock(&supp->mutex);
		if (supp->shutting_down) {
			mutex_unlock(&supp->mutex);
			ret = -ESHUTDOWN;
			goto err;
		}

		req = list_first_entry_or_null(&supp->reqs,
					       struct mbedtee_supp_req, node);
		if (!req) {
			mutex_unlock(&supp->mutex);
			if (wait_for_completion_interruptible(&supp->reqs_c)) {
				ret = -ERESTARTSYS;
				goto err;
			}
			continue;
		}

		list_del_init(&req->node);
		req->worker = current;
		req->ctx = ctx;
		list_add_tail(&req->node, &supp->active_reqs);

		if (req->size > param->u.memref.size) {
			/*
			 * The supplicant cannot grow its buffer dynamically, so
			 * requeueing would retry this request forever. Complete it
			 * with a transport error and keep this ioctl waiting for the
			 * next request instead of terminating a supplicant worker.
			 */
			mbedtee_supp_complete_req(req);
			mutex_unlock(&supp->mutex);
			continue;
		}

		req->shm = shm;
		*func = req->func;
		memcpy(shm->kaddr, req->data, req->size);
		param->u.memref.size = req->size;
		mutex_unlock(&supp->mutex);

		return 0;
	}

err:
	tee_shm_put(shm);
	param->u.memref.shm = NULL;
	return ret;
}

/*
 * Called by supplicant to send back the result of a request.
 */
int mbedtee_supp_send(struct tee_context *ctx, u32 ret, u32 num_params,
		      struct tee_param *param)
{
	struct tee_device *teedev = ctx->teedev;
	struct mbedtee_device *mbedtee = tee_get_drvdata(teedev);
	struct mbedtee_supp *supp = &mbedtee->supp;
	struct mbedtee_supp_req *req;
	struct mbedtee_supp_req *_req;
	bool invalid;

	/*
	 * Find and detach the active request belonging to this worker thread
	 * and to this supplicant context. Each worker is identified by its
	 * task_struct pointer, which was recorded when supp_recv() dispatched
	 * the request.
	 */
	mutex_lock(&supp->mutex);
	req = NULL;
	list_for_each_entry(_req, &supp->active_reqs, node) {
		if (_req->worker == current && _req->ctx == ctx) {
			req = _req;
			list_del_init(&req->node);
			break;
		}
	}
	mutex_unlock(&supp->mutex);

	if (!req)
		return -ENOENT;

	/*
	 * Complete the request even on malformed input: the worker waiting
	 * in mbedtee_supp_handler() must not hang because a broken
	 * supplicant sent invalid parameters.
	 */
	invalid = num_params != 1 || !tee_param_is_memref(param) ||
		  param->u.memref.size > req->size;
	if (invalid) {
		if (req->data && req->size >= sizeof(struct supp_cmd_hdr))
			((struct supp_cmd_hdr *)req->data)->ret =
				TEEC_ERROR_COMMUNICATION;
	} else {
		memcpy(req->data, req->shm->kaddr, param->u.memref.size);
	}
	if (req->shm)
		tee_shm_put(req->shm);
	req->shm = NULL;

	/* Wake up mbedtee_supp_handler(). */
	complete(&req->c);

	return invalid ? -EINVAL : 0;
}
