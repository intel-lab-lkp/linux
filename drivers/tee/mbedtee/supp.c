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
	int ret;
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
	r->ret = TEEC_ERROR_COMMUNICATION;
	complete(&r->c);
}

void mbedtee_supp_release(struct mbedtee_supp *supp, struct tee_context *ctx)
{
	struct mbedtee_supp_req *r, *n;

	mutex_lock(&supp->mutex);

	/*
	 * Cancel only requests owned by @ctx (or unclaimed ones).
	 * Requests owned by other supplicant contexts are left alone
	 * so that independent worker threads on separate fds do not
	 * interfere with each other.
	 */
	list_for_each_entry_safe(r, n, &supp->reqs, node) {
		if (r->ctx && r->ctx != ctx)
			continue;
		mbedtee_supp_complete_req(r);
	}

	list_for_each_entry_safe(r, n, &supp->active_reqs, node) {
		if (r->ctx && r->ctx != ctx)
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

	wait_for_completion(&req->c);

	kfree(req);
}

static int supp_pop_req(struct mbedtee_supp *supp,
			struct mbedtee_supp_req **req)
{
	struct mbedtee_supp_req *r = NULL;
	int ret = 0;

	mutex_lock(&supp->mutex);
	if (supp->shutting_down) {
		ret = -ESHUTDOWN;
		goto out;
	}

	r = list_first_entry_or_null(&supp->reqs, struct mbedtee_supp_req, node);

	if (r) {
		list_del_init(&r->node);
		r->worker = current;
		list_add_tail(&r->node, &supp->active_reqs);
	}

out:
	mutex_unlock(&supp->mutex);
	*req = r;
	return ret;
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
	struct tee_shm *shm = param->u.memref.shm;
	int ret;

	ret = mbedtee_supp_check_recv_params(*num_params, param);
	if (ret != 0)
		return ret;

	shm = tee_shm_get_from_id(ctx, shm->id);
	tee_shm_put(param->u.memref.shm);
	if (IS_ERR(shm))
		return PTR_ERR(shm);
	param->u.memref.shm = shm;

	d->is_supp_ctx = true;

	while (true) {
		ret = supp_pop_req(supp, &req);
		if (ret != 0)
			goto err;
		if (req)
			break;

		if (wait_for_completion_interruptible(&supp->reqs_c)) {
			ret = -ERESTARTSYS;
			goto err;
		}
	}

	/* Record which supplicant context owns this request. */
	req->ctx = ctx;

	if (req->size > param->u.memref.size) {
		/* Return the request to the queue so it isn't lost */
		mutex_lock(&supp->mutex);
		list_del(&req->node);
		list_add(&req->node, &supp->reqs);
		mutex_unlock(&supp->mutex);
		ret = -EOVERFLOW;
		goto err;
	}

	*func = req->func;
	req->shm = shm;

	memcpy(shm->kaddr, req->data, req->size);
	param->u.memref.size = req->size;

	return 0;

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

	if (num_params != 1)
		return -EINVAL;

	if (!tee_param_is_memref(param))
		return -EINVAL;

	/*
	 * Find the active request belonging to this worker thread.
	 * Each worker is identified by its task_struct pointer, which
	 * was recorded when supp_pop_req() dispatched the request.
	 */
	mutex_lock(&supp->mutex);
	req = NULL;
	list_for_each_entry(_req, &supp->active_reqs, node) {
		if (_req->worker == current) {
			req = _req;
			list_del_init(&req->node);
			break;
		}
	}
	mutex_unlock(&supp->mutex);

	if (!req)
		return -ENOENT;

	if (param->u.memref.size > req->size) {
		req->ret = TEEC_ERROR_COMMUNICATION;
	} else {
		req->ret = ret;
		memcpy(req->data, req->shm->kaddr, param->u.memref.size);
	}
	if (req->shm)
		tee_shm_put(req->shm);
	req->shm = NULL;

	/* Wake up mbedtee_supp_handler(). */
	complete(&req->c);

	return 0;
}
