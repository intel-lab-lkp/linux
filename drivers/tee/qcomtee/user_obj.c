// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "qcomtee_private.h"

/**
 * DOC: User Objects aka Supplicants
 *
 * Any userspace process with access to the TEE device file can behave as a supplicant
 * by creating a user object. Any TEE parameter of type OBJREF with %QCOM_TEE_OBJREF_FLAG_USER
 * flag set is considered as user object.
 *
 * A supplicant uses qcom_tee_user_object_pick() (i.e. TEE_IOC_SUPPL_RECV) to receive a
 * QTEE user object request and qcom_tee_user_object_submit() (i.e. TEE_IOC_SUPPL_SEND)
 * to submit a response. QTEE expects to receive the response, including OB and OO in
 * specific order in the message; parameters submitted with qcom_tee_user_object_submit()
 * should maintain this order.
 */

/**
 * struct qcom_tee_user_object - User object.
 * @object: &struct qcom_tee_object representing this user object.
 * @ctx: context for which user object is defined.
 * @object_id: object ID in @ctx.
 * @nor: notify userspace if object is released.
 *
 * Any object managed in userspace is represented with this struct.
 * If @nor is set, on release a notification message is send back to the userspace.
 */
struct qcom_tee_user_object {
	struct qcom_tee_object object;
	struct qcom_tee_context *ctx;
	u64 object_id;

	bool nor;
};

#define to_qcom_tee_user_object(o) container_of((o), struct qcom_tee_user_object, object)

static struct qcom_tee_object_operations qcom_tee_user_object_ops;

/* Is it a user object? */
int is_qcom_tee_user_object(struct qcom_tee_object *object)
{
	return object != NULL_QCOM_TEE_OBJECT &&
		typeof_qcom_tee_object(object) == QCOM_TEE_OBJECT_TYPE_CB_OBJECT &&
		object->ops == &qcom_tee_user_object_ops;
}

/* Set user object's 'notify on release' flag. */
void qcom_tee_user_object_set_notify(struct qcom_tee_object *object, bool notify)
{
	if (is_qcom_tee_user_object(object))
		WRITE_ONCE(to_qcom_tee_user_object(object)->nor, notify);
}

/**
 * enum qcom_tee_req_state - Current state of request.
 * @QCOM_TEE_REQ_QUEUED: Request is waiting for supplicant.
 * @QCOM_TEE_REQ_PROCESSING: Request has been picked by the supplicant.
 * @QCOM_TEE_REQ_PROCESSED: Response has been submitted for the request.
 */
enum qcom_tee_req_state {
	QCOM_TEE_REQ_QUEUED = 1,
	QCOM_TEE_REQ_PROCESSING,
	QCOM_TEE_REQ_PROCESSED,
};

/* User requests sent to supplicants. */
struct qcom_tee_user_req {
	enum qcom_tee_req_state state;

	int req_id;			/* Request ID. */
	u64 object_id;			/* User object ID. */
	u32 op;				/* Operation to perform on object. */
	struct qcom_tee_arg *args;	/* QTEE arguments for this operation. */
	int errno;			/* Result of operation. */

	struct completion c;		/* Completion for whoever wait for results. */
};

/* Static placeholder for a request in PROCESSING state in qcom_tee_context.reqs_idr.
 * If the thread initiated the QTEE call using qcom_tee_object_invoke() dies, and supplicant
 * is processing the request, we replace the entry in qcom_tee_context.reqs_idr with
 * __empty_ureq. So (1) the req_id remains busy and not reused, and (2) supplicant fails to
 * submit response and does the necessary rollback.
 */
static struct qcom_tee_user_req __empty_ureq = { .state = QCOM_TEE_REQ_PROCESSING };

/* Enqueue a user request for a context. */
static int qcom_tee_request_enqueue(struct qcom_tee_user_req *ureq, struct qcom_tee_context *ctx)
{
	int ret;

	guard(mutex)(&ctx->lock);
	/* Supplicant is dying. */
	if (ctx->released)
		return -ENODEV;

	ret = idr_alloc(&ctx->reqs_idr, ureq, 0, 0, GFP_KERNEL);
	if (ret < 0)
		return ret;

	ureq->req_id = ret;
	ureq->state = QCOM_TEE_REQ_QUEUED;

	return 0;
}

/**
 * qcom_tee_requests_destroy() - Destroy requests in a context.
 * @ctx: context for which to destroy requests.
 */
void qcom_tee_requests_destroy(struct qcom_tee_context *ctx)
{
	struct qcom_tee_user_req *ureq;
	int id;

	guard(mutex)(&ctx->lock);

	/* So qcom_tee_request_enqueue() refuses new requests. */
	ctx->released = 1;
	idr_for_each_entry(&ctx->reqs_idr, ureq, id) {
		if (ureq == &__empty_ureq)
			continue;
		/* ureq in QUEUED or PROCESSING state, terminate them. */
		if (ureq->op == QCOM_TEE_OBJREF_OP_RELEASE) {
			kfree(ureq);
		} else {
			ureq->state = QCOM_TEE_REQ_PROCESSED;
			ureq->errno = -ENODEV;
			complete(&ureq->c);
		}
	}
}

/**
 * qcom_tee_supp_pop_entry() - Pop the next request in a context.
 * @ctx: context from which to pop a request.
 * @ubuf_size: size of available buffer for MEMBUF parameters.
 * @num_params: number of entries for TEE parameter array.
 *
 * It does not remove the request from &qcom_tee_context.reqs_idr.
 * It checks if @num_params is large enough to fit the next request arguments.
 * It checks if @ubuf_size is large enough to fit IB buffer arguments from QTEE.
 * It updates request state to %QCOM_TEE_REQ_PROCESSING state.
 *
 * Return: On success return a request or NULL and ERR_PTR on failure.
 */
static struct qcom_tee_user_req *qcom_tee_supp_pop_entry(struct qcom_tee_context *ctx,
							 size_t ubuf_size, int num_params)
{
	struct qcom_tee_user_req *ureq;
	struct qcom_tee_arg *u;
	int i, id;

	guard(mutex)(&ctx->lock);

	/* Find the a QUEUED request. */
	idr_for_each_entry(&ctx->reqs_idr, ureq, id)
		if (ureq->state == QCOM_TEE_REQ_QUEUED)
			break;

	if (!ureq)
		return NULL;

	u = ureq->args;
	/* (1) Is there enough TEE parameters? */
	if (num_params < qcom_tee_args_len(u))
		return ERR_PTR(-EINVAL);

	/* (2) Is there enough space to pass input buffers? */
	qcom_tee_arg_for_each_input_buffer(i, u) {
		ubuf_size = size_sub(ubuf_size, u[i].b.size);
		if (ubuf_size == SIZE_MAX)
			return ERR_PTR(-EINVAL);

		ubuf_size = round_down(ubuf_size, 8);
	}

	/* Ready to process request 'QUEUED -> PROCESSING'. */
	ureq->state = QCOM_TEE_REQ_PROCESSING;

	return ureq;
}

/* User object dispatcher. */
static int qcom_tee_user_object_dispatch(struct qcom_tee_object_invoke_ctx *oic,
					 struct qcom_tee_object *object, u32 op,
					 struct qcom_tee_arg *args)
{
	struct qcom_tee_user_object *uo = to_qcom_tee_user_object(object);
	struct qcom_tee_user_req *ureq __free(kfree);
	struct qcom_tee_context *ctx = uo->ctx;
	int errno;

	ureq = kzalloc(sizeof(*ureq), GFP_KERNEL);
	if (!ureq)
		return -ENOMEM;

	init_completion(&ureq->c);
	ureq->object_id = uo->object_id;
	ureq->op = op;
	ureq->args = args;

	/* Queue the request. */
	if (qcom_tee_request_enqueue(ureq, ctx))
		return -ENODEV;

	/* Wakeup supplicant to process it. */
	complete(&ctx->req_c);

	/* Wait for supplicant to process the request. */
	/* Supplicant is expected to process request in a timely manner. We wait as KILLABLE,
	 * in case supplicant and invoke thread both running from a same user process, otherwise
	 * the process stuck on fatal signal.
	 */
	if (!wait_for_completion_state(&ureq->c, TASK_KILLABLE | TASK_FREEZABLE)) {
		errno = ureq->errno;
		/* On SUCCESS, end_cb_notify frees the request. */
		if (!errno)
			oic->data = no_free_ptr(ureq);
	} else {
		enum qcom_tee_req_state prev_state;

		errno = -ENODEV;

		scoped_guard(mutex, &ctx->lock) {
			prev_state = ureq->state;
			/* Replace ureq with '__empty_ureq' to keep req_id reserved. */
			if (prev_state == QCOM_TEE_REQ_PROCESSING)
				idr_replace(&ctx->reqs_idr, &__empty_ureq, ureq->req_id);
			/* Remove ureq as supplicant has never seen this request. */
			else if (prev_state == QCOM_TEE_REQ_QUEUED)
				idr_remove(&ctx->reqs_idr, ureq->req_id);
		}

		/* Wait for exclusive access to ureq. */
		synchronize_srcu(&ctx->req_srcu);

		/* Supplicant did some work for us, we should not discard it. */
		if (prev_state == QCOM_TEE_REQ_PROCESSED) {
			errno = ureq->errno;
			/* On SUCCESS, end_cb_notify frees the request. */
			if (!errno)
				oic->data = no_free_ptr(ureq);
		}
	}

	return errno;
}

/* Called after submitting the callback response. */
static void qcom_tee_user_object_end_cb_notify(struct qcom_tee_object_invoke_ctx *oic,
					       struct qcom_tee_object *unused_object, int err)
{
	struct qcom_tee_user_req *ureq = oic->data;
	struct qcom_tee_arg *u = ureq->args;
	struct qcom_tee_object *object;
	int i;

	qcom_tee_arg_for_each_output_object(i, u) {
		object = u[i].o;
		/* If err, drop QTEE copy otherwise just drop driver's copy. */
		if (err && (typeof_qcom_tee_object(object) == QCOM_TEE_OBJECT_TYPE_CB_OBJECT))
			qcom_tee_object_put(object);
		qcom_tee_object_put(object);
	}

	kfree(ureq);
}

static void qcom_tee_user_object_release(struct qcom_tee_object *object)
{
	struct qcom_tee_user_object *uo = to_qcom_tee_user_object(object);
	struct qcom_tee_context *ctx = uo->ctx;
	struct qcom_tee_user_req *ureq;

	static struct qcom_tee_arg args[] = { { .type = QCOM_TEE_ARG_TYPE_INV } };

	if (READ_ONCE(uo->nor)) {
		ureq = kzalloc(sizeof(*ureq), GFP_KERNEL);
		if (ureq) {
			ureq->object_id = uo->object_id;
			ureq->op = QCOM_TEE_OBJREF_OP_RELEASE;
			ureq->args = args;

			/* Queue the RELEASE request and wake supplicant to process it. */
			qcom_tee_request_enqueue(ureq, ctx);

			complete(&ctx->req_c);
		} else {
			pr_err("failed to notify user object (%s) release.\n",
			       qcom_tee_object_name(object));
		}
	}

	/* Matching get is in qcom_tee_user_param_to_object(). */
	kref_put(&ctx->ref_cnt, __qcom_tee_context_destroy);
	kfree(uo);
}

static struct qcom_tee_object_operations qcom_tee_user_object_ops = {
	.release = qcom_tee_user_object_release,
	.notify = qcom_tee_user_object_end_cb_notify,
	.dispatch = qcom_tee_user_object_dispatch,
};

/**
 * qcom_tee_user_param_to_object() - Convert OBJREF parameter to &struct qcom_tee_object.
 * @object: object returned.
 * @param: TEE parameter.
 * @ctx: context in which the conversion should happen.
 *
 * @param is OBJREF with %QCOM_TEE_OBJREF_FLAG_USER flags.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcom_tee_user_param_to_object(struct qcom_tee_object **object, struct tee_param *param,
				  struct qcom_tee_context *ctx)
{
	struct qcom_tee_user_object *user_object __free(kfree) = NULL;
	struct qcom_tee_object *uo;
	int err;

	user_object = kzalloc(sizeof(*user_object), GFP_KERNEL);
	if (!user_object)
		return -ENOMEM;

	user_object->ctx = ctx;
	user_object->object_id = param->u.objref.id;
	/* By default, always notify userspace on release. */
	user_object->nor = true;

	err = qcom_tee_object_user_init(&user_object->object, QCOM_TEE_OBJECT_TYPE_CB_OBJECT,
					&qcom_tee_user_object_ops, "uo-%lu", param->u.objref.id);
	if (err)
		return err;

	uo = &no_free_ptr(user_object)->object;
	/* Keep context alive as user object is alive. */
	kref_get(&ctx->ref_cnt);

	*object = uo;

	return 0;
}

/* Reverse what qcom_tee_user_param_to_object() does. */
int qcom_tee_user_param_from_object(struct tee_param *param, struct qcom_tee_object *object,
				    struct qcom_tee_context *ctx)
{
	struct qcom_tee_user_object *uo;

	if (!is_qcom_tee_user_object(object))
		return -EINVAL;

	uo = to_qcom_tee_user_object(object);
	/* Sure if the object is in a same context as caller? */
	if (uo->ctx != ctx)
		return -EINVAL;

	param->u.objref.id = uo->object_id;
	param->u.objref.flags = QCOM_TEE_OBJREF_FLAG_USER;

	/* User objects are valid in the context of userspace; drop the driver copy. */
	qcom_tee_object_put(object);

	return 0;
}

/**
 * qcom_tee_cb_params_from_args() - Convert QTEE arguments to TEE parameters in a context.
 * @params: TEE parameters.
 * @u: QTEE arguments.
 * @num_params: number of elements in the parameter array.
 * @ubuf_addr: user buffer for argument of type %QCOM_TEE_ARG_TYPE_IB.
 * @ubuf_size: size of user buffer.
 * @ctx: context in which the conversion should happen.
 *
 * It expects @params to have enough entries for @u. Entries in @params are of
 * %TEE_IOCTL_PARAM_ATTR_TYPE_NONE. On failure, it puts IO objects.
 *
 * Return: On success return number of input parameters processed or <0 on failure.
 */
static int qcom_tee_cb_params_from_args(struct tee_param *params,
					struct qcom_tee_arg *u, int num_params,
					void __user *ubuf_addr, size_t ubuf_size,
					struct qcom_tee_context *ctx)
{
	int i, np = 0;

	qcom_tee_arg_for_each(i, u) {
		enum qcom_tee_arg_type at = u[i].type;

		if (at == QCOM_TEE_ARG_TYPE_IB) {
			params[np].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_INPUT;

			/* Underflow already checked in qcom_tee_supp_pop_entry(). */
			ubuf_size = round_down(ubuf_size - u[i].b.size, 8);
			params[np].u.membuf.uaddr = (void * __user)(ubuf_addr + ubuf_size);
			params[np].u.membuf.size = u[i].b.size;
			if (copy_to_user(params[np].u.membuf.uaddr, u[i].b.addr, u[i].b.size))
				goto out_failed;

			np++;
		} else if (at == QCOM_TEE_ARG_TYPE_IO) {
			params[np].attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT;
			if (qcom_tee_objref_from_arg(&params[np], &u[i], ctx))
				goto out_failed;

			np++;
		}
	}

	return np;

out_failed:

	/* On ERROR: */
	/*  - Release params associated to QTEE objects in this context so far. */
	for (; np >= 0; np--) {
		if (params[np].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT)
			qcom_tee_context_del_qtee_object(&params[np], ctx);
	}
	/*  - Release any IO objects not processed so far. */
	while (u[i].type) {
		if (u[i].type == QCOM_TEE_ARG_TYPE_IO)
			qcom_tee_object_put(u[i++].o);
	}

	return -EINVAL;
}

/**
 * qcom_tee_cb_params_to_args() - Convert TEE parameters to QTEE arguments in a context.
 * @u: QTEE arguments.
 * @params: TEE parameters.
 * @num_params: number of elements in the parameter array.
 * @ctx: context in which the conversion should happen.
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_cb_params_to_args(struct qcom_tee_arg *u,
				      struct tee_param *params, int num_params,
				      struct qcom_tee_context *ctx)
{
	struct qcom_tee_object *object;
	int i, np = 0;

	qcom_tee_arg_for_each(i, u) {
		enum qcom_tee_arg_type at = u[i].type;

		if (at == QCOM_TEE_ARG_TYPE_OB) {
			if (params[np].attr != TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_OUTPUT ||
			    params[np].u.membuf.size > u[i].b.size)
				goto out_failed;

			if (copy_from_user(u[i].b.addr, params[np].u.membuf.uaddr, u[i].b.size))
				goto out_failed;

			u[i].b.size = params[np].u.membuf.size;

			np++;
		} else if (at == QCOM_TEE_ARG_TYPE_OO) {
			if (params[np].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT) {
				if (qcom_tee_objref_to_arg(&u[i], &params[np], ctx))
					goto out_failed;
			} else {
				goto out_failed;
			}

			np++;
		}
	}

	return 0;

out_failed:

	/* On ERROR: */
	/* Put OO objects processed so far. */
	for (--i; i >= 0; i--) {
		if (u[i].type != QCOM_TEE_ARG_TYPE_OO)
			continue;

		object = u[i].o;
		qcom_tee_user_object_set_notify(object, false);
		/* For callback object, we hold a reference for the driver, put it. */
		if (typeof_qcom_tee_object(object) == QCOM_TEE_OBJECT_TYPE_CB_OBJECT)
			qcom_tee_object_put(object);
		/* Put QTEE copy of object. */
		qcom_tee_object_put(object);
	}

	return -EINVAL;
}

/**
 * qcom_tee_user_object_pop() - Pop a request for a user object.
 * @ctx: context to look for user object.
 * @params: parameters for @op.
 * @num_params: number of elements in the parameter array.
 * @uaddr: user buffer for output MEMBUF parameters.
 * @size: size of user buffer @uaddr.
 * @data: information for the pop request.
 *
 * @params is filled along with @data for the picked request.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcom_tee_user_object_pop(struct qcom_tee_context *ctx,
			     struct tee_param *params, int num_params,
			     void __user *uaddr, size_t size,
			     struct qcom_tee_user_object_request_data *data)
{
	struct qcom_tee_user_req *ureq;
	struct qcom_tee_arg *req_args;
	u64 req_object_id;
	u32 req_op;
	int req_id;
	int ret;

	while (1) {
		scoped_guard(srcu, &ctx->req_srcu) {
			/* Pop a request 'QUEUED -> PROCESSING'. */
			ureq = qcom_tee_supp_pop_entry(ctx, size, num_params);
			if (!ureq)
				goto wait_for_request;

			/* On failure, issue with params, e.g. not enough space in user buffer. */
			if (IS_ERR(ureq))
				return PTR_ERR(ureq);

			/* ''Prepare user request:'' */
			req_id = ureq->req_id;
			req_object_id = ureq->object_id;
			req_op = ureq->op;
			req_args = ureq->args;
			ret = qcom_tee_cb_params_from_args(params, req_args,
							   num_params, uaddr,
							   size, ctx);
		}

		if (ret >= 0)
			break;

		/* On failure, issue with req_args, e.g. invalid object. */
		scoped_guard(mutex, &ctx->lock) {
			/* If (!= __empty_req) then 'PROCESSING -> PROCESSED'. */
			if (idr_remove(&ctx->reqs_idr, req_id) == ureq) {
				ureq->state = QCOM_TEE_REQ_PROCESSED;
				ureq->errno = ret;
				/* Send error to QTEE. */
				complete(&ureq->c);
			}
		}

		/* Try next request. */
		continue;

wait_for_request:
		/* Wait for a new QUEUED request. */
		if (wait_for_completion_interruptible(&ctx->req_c))
			return -ERESTARTSYS;
	}

	/* It is a RELEASE message; no one is waiting for result. */
	if (req_op == QCOM_TEE_OBJREF_OP_RELEASE) {
		scoped_guard(mutex, &ctx->lock)
			idr_remove(&ctx->reqs_idr, req_id);
		kfree(ureq);
	}

	/* ''Pick a pending request:'' */
	data->id = req_id;
	data->object_id = req_object_id;
	data->op = req_op;
	data->np = ret;

	return 0;
}

/**
 * qcom_tee_user_object_submit() - Submit a response for a user object.
 * @ctx: context to look for user object.
 * @params: returned parameters.
 * @num_params: number of elements in the parameter array.
 * @id: request id for the response.
 * @errno: result of user object invocation.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcom_tee_user_object_submit(struct qcom_tee_context *ctx,
				struct tee_param *params, int num_params, int id, int errno)
{
	struct qcom_tee_user_req *ureq;

	scoped_guard(srcu, &ctx->req_srcu) {
		scoped_guard(mutex, &ctx->lock) {
			ureq = idr_remove(&ctx->reqs_idr, id);
			/* Is id invalid, or no one is waiting on response. */
			if (ureq == &__empty_ureq || !ureq)
				return -ENODEV;

			ureq->state = QCOM_TEE_REQ_PROCESSED;
		}

		ureq->errno = errno;
		/* Process params only on SUCCESS. */
		if (!errno)
			ureq->errno = qcom_tee_cb_params_to_args(ureq->args, params,
								 num_params, ctx);

		errno = (!errno && ureq->errno) ? ureq->errno : 0;
		/* Send result to QTEE. */
		complete(&ureq->c);
	}

	return errno;
}
