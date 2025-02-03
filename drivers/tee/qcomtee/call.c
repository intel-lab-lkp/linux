// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/tee.h>
#include <linux/platform_device.h>

#include "qcomtee_private.h"
#include "qcomtee_msg.h" // For QCOMTEE_MSG_ERROR_UNAVAIL.

/**
 * enum qcomtee_object_host - Object host where it is managed.
 * @QCOMTEE_OBJECT_HOST_USER: objects in userspace.
 * @QCOMTEE_OBJECT_HOST_TEE: objects in QTEE.
 *
 * We refer to objects hosted in userspace as 'Local Objects' and objects
 * hosted in QTEE as 'Remote Objects'.
 */
enum qcomtee_object_host {
	QCOMTEE_OBJECT_HOST_USER, /* Object that is managed in userspace. */
	QCOMTEE_OBJECT_HOST_TEE, /* Object that is managed in QTEE. */
};

/* Read object host information. */
static enum qcomtee_object_host qcomtee_object_host(struct tee_param *param)
{
	if (param->u.objref.flags & QCOMTEE_OBJREF_FLAG_USER)
		return QCOMTEE_OBJECT_HOST_USER;
	return QCOMTEE_OBJECT_HOST_TEE;
}

static int find_qtee_object(struct qcomtee_object **object, unsigned long id,
			    struct qcomtee_context_data *ctxdata)
{
	int err = 0;

	guard(rcu)();
	/* Object release is RCU protected. */
	*object = idr_find(&ctxdata->qtee_objects_idr, id);
	if (!qcomtee_object_get(*object))
		err = -EINVAL;

	return err;
}

static void del_qtee_object(unsigned long id,
			    struct qcomtee_context_data *ctxdata)
{
	struct qcomtee_object *object;

	scoped_guard(spinlock, &ctxdata->qtee_lock)
		object = idr_remove(&ctxdata->qtee_objects_idr, id);

	qcomtee_object_put(object);
}

/**
 * qcomtee_context_add_qtee_object() - Add a QTEE object to the context.
 * @param: TEE parameter representing @object.
 * @object: QTEE object.
 * @ctx: context to add the object.
 *
 * It assumes @object is %QCOMTEE_OBJECT_TYPE_TEE and the caller has already
 * issued qcomtee_object_get() for @object.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
int qcomtee_context_add_qtee_object(struct tee_param *param,
				    struct qcomtee_object *object,
				    struct tee_context *ctx)
{
	int ret;
	struct qcomtee_context_data *ctxdata = ctx->data;

	guard(spinlock)(&ctxdata->qtee_lock);
	ret = idr_alloc(&ctxdata->qtee_objects_idr, object, 0, 0, GFP_KERNEL);
	if (ret < 0)
		return ret;

	param->u.objref.id = ret;
	/* QTEE Object: !QCOMTEE_OBJREF_FLAG_USER. */
	param->u.objref.flags = 0;

	return 0;
}

/* Retrieve the QTEE object added with qcomtee_context_add_qtee_object(). */
int qcomtee_context_find_qtee_object(struct qcomtee_object **object,
				     struct tee_param *param,
				     struct tee_context *ctx)
{
	struct qcomtee_context_data *ctxdata = ctx->data;

	/* 'qtee_objects_idr' stores QTEE objects only. */
	if (qcomtee_object_host(param) != QCOMTEE_OBJECT_HOST_TEE)
		return -EINVAL;

	return find_qtee_object(object, param->u.objref.id, ctxdata);
}

/**
 * qcomtee_context_del_qtee_object() - Delete a QTEE object from the context.
 * @param: TEE parameter representing @object.
 * @ctx: context for deleting the object.
 *
 * The @param has been initialized by qcomtee_context_add_qtee_object().
 */
void qcomtee_context_del_qtee_object(struct tee_param *param,
				     struct tee_context *ctx)
{
	struct qcomtee_context_data *ctxdata = ctx->data;

	/* 'qtee_objects_idr' stores QTEE objects only. */
	if (qcomtee_object_host(param) == QCOMTEE_OBJECT_HOST_TEE)
		del_qtee_object(param->u.objref.id, ctxdata);
}

/**
 * qcomtee_objref_to_arg() - Convert OBJREF parameter to QTEE argument.
 * @arg: QTEE argument.
 * @param: TEE parameter.
 * @ctx: context in which the conversion should happen.
 *
 * It assumes @param is an OBJREF.
 * It does not set @arg.type; the caller should initialize it to a correct
 * &enum qcomtee_arg_type value. It gets the object's refcount in @arg;
 * the caller should manage to put it afterward.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
int qcomtee_objref_to_arg(struct qcomtee_arg *arg, struct tee_param *param,
			  struct tee_context *ctx)
{
	struct qcomtee_object *object;
	int err;

	if (arg->type != QCOMTEE_ARG_TYPE_IO &&
	    arg->type != QCOMTEE_ARG_TYPE_OO)
		return -EINVAL;

	/* It is a NULL object?! */
	if (param->u.objref.id == TEE_OBJREF_NULL) {
		arg->o = NULL_QCOMTEE_OBJECT;

		return 0;
	}

	switch (qcomtee_object_host(param)) {
	case QCOMTEE_OBJECT_HOST_USER:
		err = qcomtee_user_param_to_object(&object, param, ctx);
		if (err)
			break;

		/*
		 * QTEE may release an object using an async message.
		 * The async message may release this object even before the
		 * invocation returns. Keep a copy for the driver to be safe.
		 */
		qcomtee_object_get(object);

		break;
	case QCOMTEE_OBJECT_HOST_TEE:
		err = qcomtee_context_find_qtee_object(&object, param, ctx);

		break;
	}

	arg->o = err ? NULL_QCOMTEE_OBJECT : object;

	return err;
}

/**
 * qcomtee_objref_from_arg() - Convert QTEE argument to OBJREF param.
 * @param: TEE parameter.
 * @arg: QTEE argument.
 * @ctx: context in which the conversion should happen.
 *
 * It assumes @arg is of %QCOMTEE_ARG_TYPE_IO or %QCOMTEE_ARG_TYPE_OO.
 * It does not set @param.attr; the caller should initialize it to a
 * correct type.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
int qcomtee_objref_from_arg(struct tee_param *param, struct qcomtee_arg *arg,
			    struct tee_context *ctx)
{
	struct qcomtee_object *object;
	int err;

	/* param should be of OBJREF. */
	if (param->attr != TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT &&
	    param->attr != TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT &&
	    param->attr != TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INOUT)
		return -EINVAL;

	object = arg->o;

	switch (typeof_qcomtee_object(object)) {
	case QCOMTEE_OBJECT_TYPE_NULL:
		param->u.objref.id = TEE_OBJREF_NULL;
		err = 0;

		break;
	case QCOMTEE_OBJECT_TYPE_CB:
		err = qcomtee_user_param_from_object(param, object, ctx);

		break;
	case QCOMTEE_OBJECT_TYPE_TEE:
		err = qcomtee_context_add_qtee_object(param, object, ctx);

		break;
	case QCOMTEE_OBJECT_TYPE_ROOT:
	default:
		return -EINVAL;
	}

	return err;
}

/**
 * qcomtee_params_to_args() - Convert TEE parameters to QTEE arguments.
 * @u: QTEE arguments.
 * @params: TEE parameters.
 * @num_params: number of elements in the parameter array.
 * @ctx: context in which the conversion should happen.
 *
 * It assumes @u has at least @num_params + 1 entries and has been initialized
 * with %QCOMTEE_ARG_TYPE_INV as &struct qcomtee_arg.type.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
static int qcomtee_params_to_args(struct qcomtee_arg *u,
				  struct tee_param *params, int num_params,
				  struct tee_context *ctx)
{
	int i;

	for (i = 0; i < num_params; i++) {
		switch (params[i].attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT:
			u[i].flags = QCOMTEE_ARG_FLAGS_UADDR;
			u[i].b.uaddr = params[i].u.ubuf.uaddr;
			u[i].b.size = params[i].u.ubuf.size;

			if (params[i].attr ==
			    TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT)
				u[i].type = QCOMTEE_ARG_TYPE_IB;
			else /* TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT */
				u[i].type = QCOMTEE_ARG_TYPE_OB;

			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT:
			u[i].type = QCOMTEE_ARG_TYPE_IO;
			if (qcomtee_objref_to_arg(&u[i], &params[i], ctx))
				goto out_failed;

			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT:
			u[i].type = QCOMTEE_ARG_TYPE_OO;
			u[i].o = NULL_QCOMTEE_OBJECT;
			break;
		default:
			goto out_failed;
		}
	}

	return 0;

out_failed:
	/* Undo qcomtee_objref_to_arg(). */
	for (i--; i >= 0; i--) {
		if (u[i].type != QCOMTEE_ARG_TYPE_IO)
			continue;

		qcomtee_user_object_set_notify(u[i].o, false);
		if (typeof_qcomtee_object(u[i].o) == QCOMTEE_OBJECT_TYPE_CB)
			qcomtee_object_put(u[i].o);

		qcomtee_object_put(u[i].o);
	}

	return -EINVAL;
}

/**
 * qcomtee_params_from_args() - Convert QTEE arguments to TEE parameters.
 * @params: TEE parameters.
 * @u: QTEE arguments.
 * @num_params: number of elements in the parameter array.
 * @ctx: context in which the conversion should happen.
 *
 * @u should have already been initialized by qcomtee_params_to_args().
 * This also represents the end of a QTEE invocation that started with
 * qcomtee_params_to_args() by releasing %QCOMTEE_ARG_TYPE_IO objects.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
static int qcomtee_params_from_args(struct tee_param *params,
				    struct qcomtee_arg *u, int num_params,
				    struct tee_context *ctx)
{
	int i, np;

	qcomtee_arg_for_each(np, u) {
		if (u[np].type == QCOMTEE_ARG_TYPE_OB) {
			/* TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT */
			params[np].u.value.b = u[np].b.size;

		} else if (u[np].type == QCOMTEE_ARG_TYPE_IO) {
			/* IEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT */
			qcomtee_object_put(u[np].o);

		} else if (u[np].type == QCOMTEE_ARG_TYPE_OO) {
			/* TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT */
			if (qcomtee_objref_from_arg(&params[np], &u[np], ctx))
				goto out_failed;
		}
	}

	return 0;

out_failed:
	/* Undo qcomtee_objref_from_arg(). */
	for (i = 0; i < np; i++) {
		if (params[i].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT)
			qcomtee_context_del_qtee_object(&params[i], ctx);
	}

	/* Release any IO and OO objects not processed. */
	for (; u[i].type; i++) {
		if (u[i].type == QCOMTEE_ARG_TYPE_OO ||
		    u[i].type == QCOMTEE_ARG_TYPE_IO)
			qcomtee_object_put(u[i].o);
	}

	return -EINVAL;
}

/* TEE Device Ops. */

static int qcomtee_params_check(struct tee_param *params, int num_params)
{
	int io = 0, oo = 0, ib = 0, ob = 0;
	int i;

	/* QTEE accepts 64 arguments. */
	if (num_params > QCOMTEE_ARGS_MAX)
		return -EINVAL;

	/* Supported parameter types. */
	for (i = 0; i < num_params; i++) {
		switch (params[i].attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT:
			ib++;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_OUTPUT:
			ob++;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT:
			io++;
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT:
			oo++;
			break;
		default:
			return -EINVAL;
		}
	}

	/*  QTEE accepts 16 arguments of each supported types. */
	if (io > QCOMTEE_ARGS_PER_TYPE || oo > QCOMTEE_ARGS_PER_TYPE ||
	    ib > QCOMTEE_ARGS_PER_TYPE || ob > QCOMTEE_ARGS_PER_TYPE)
		return -EINVAL;

	return 0;
}

/* Check if an operation on ROOT_QCOMTEE_OBJECT from userspace is permitted. */
static int qcomtee_root_object_check(u32 op, struct tee_param *params,
				     int num_params)
{
	/* Some privileged operations recognized by QTEE. */
	if (op == 4 || op == 8 || op == 9)
		return -EINVAL;

	/*
	 * OP 5 is to register with QTEE by passing a credential object as
	 * input OBJREF. TEE_OBJREF_NULL as a credential object represents
	 * a privileged client for QTEE and is used by the kernel only.
	 */
	if (op == 5 && num_params == 2) {
		if (params[0].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT &&
		    params[1].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT) {
			if (params[0].u.objref.id == TEE_OBJREF_NULL)
				return -EINVAL;
		}
	}

	return 0;
}

/**
 * qcomtee_object_invoke() - Invoke a QTEE object.
 * @ctx: TEE context.
 * @arg: ioctl arguments.
 * @params: parameters for the object.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
static int qcomtee_object_invoke(struct tee_context *ctx,
				 struct tee_ioctl_object_invoke_arg *arg,
				 struct tee_param *params)
{
	struct qcomtee_object_invoke_ctx *oic __free(kfree) = NULL;
	struct qcomtee_context_data *ctxdata = ctx->data;
	struct qcomtee_arg *u __free(kfree) = NULL;
	struct qcomtee_object *object;
	int i, ret, result;

	if (qcomtee_params_check(params, arg->num_params))
		return -EINVAL;

	/* First, handle reserved operations: */
	if (arg->op == QCOMTEE_OBJREF_OP_RELEASE) {
		del_qtee_object(arg->object, ctxdata);

		return 0;
	} else if (arg->op > QCOMTEE_OBJREF_OP_MIN) {
		return -EINVAL;
	}

	/* Otherwise, invoke a QTEE object: */

	oic = kzalloc(sizeof(*oic), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	/* +1 for ending QCOMTEE_ARG_TYPE_INV. */
	u = kcalloc(arg->num_params + 1, sizeof(*u), GFP_KERNEL);
	if (!u)
		return -ENOMEM;

	/* Get an object to invoke. */
	if (arg->object == TEE_OBJREF_NULL) {
		/* Use ROOT if TEE_OBJREF_NULL is invoked. */
		if (qcomtee_root_object_check(arg->op, params, arg->num_params))
			return -EINVAL;

		object = ROOT_QCOMTEE_OBJECT;
	} else if (find_qtee_object(&object, arg->object, ctxdata)) {
		return -EINVAL;
	}

	ret = qcomtee_params_to_args(u, params, arg->num_params, ctx);
	if (ret)
		goto out;

	ret = qcomtee_object_do_invoke(oic, object, arg->op, u, &result);
	if (ret) {
		qcomtee_arg_for_each_input_object(i, u) {
			qcomtee_user_object_set_notify(u[i].o, false);
			qcomtee_object_put(u[i].o);
		}

		goto out;
	}

	if (!result) {
		/* Assume service is UNAVAIL if unable to process the result. */
		if (qcomtee_params_from_args(params, u, arg->num_params, ctx))
			result = QCOMTEE_MSG_ERROR_UNAVAIL;
	} else {
		/*
		 * qcomtee_params_to_args() gets a copy of IO for the driver to
		 * make sure they do not get released while in the middle of
		 * invocation. On success (!result), qcomtee_params_from_args()
		 * puts them.
		 */
		qcomtee_arg_for_each_input_object(i, u)
			qcomtee_object_put(u[i].o);
	}

	arg->ret = result;
out:
	qcomtee_object_put(object);

	return ret;
}

/**
 * qcomtee_supp_recv() - Wait for a request for the supplicant.
 * @ctx: TEE context.
 * @op: requested operation on the object.
 * @num_params: number of elements in the parameter array.
 * @params: parameters for @op.
 *
 * The first parameter is a meta %TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT.
 * On input, it provides a user buffer. This buffer is used for parameters of
 * type %TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT in qcomtee_cb_params_from_args().
 * On output, the object ID and request ID are stored in the meta parameter.
 *
 * @num_params is updated to the number of parameters that actually exist
 * in @params on return.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
static int qcomtee_supp_recv(struct tee_context *ctx, u32 *op, u32 *num_params,
			     struct tee_param *params)
{
	struct qcomtee_user_object_request_data data;
	void __user *uaddr;
	size_t ubuf_size;
	int i, ret;

	if (!*num_params)
		return -EINVAL;

	/* We expect the first parameter to be an INOUT + meta parameter. */
	if (params->attr !=
	    (TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT | TEE_IOCTL_PARAM_ATTR_META))
		return -EINVAL;

	/* Other parameters are none. */
	for (i = 1; i < *num_params; i++)
		if (params[i].attr)
			return -EINVAL;

	if (!IS_ALIGNED(params->u.value.a, 8))
		return -EINVAL;

	/* User buffer and size from meta parameter. */
	uaddr = u64_to_user_ptr(params->u.value.a);
	ubuf_size = params->u.value.b;
	/* Process TEE parameters. +/-1 to ignore the meta parameter. */
	ret = qcomtee_user_object_select(ctx, params + 1, *num_params - 1,
					 uaddr, ubuf_size, &data);
	if (ret)
		return ret;

	params->u.value.a = data.object_id;
	params->u.value.b = data.id;
	params->u.value.c = 0;
	*op = data.op;
	*num_params = data.np + 1;

	return 0;
}

/**
 * qcomtee_supp_send() - Submit a response for a request.
 * @ctx: TEE context.
 * @errno: return value for the request.
 * @num_params: number of elements in the parameter array.
 * @params: returned parameters.
 *
 * The first parameter is a meta %TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT.
 * It specifies the request ID this response belongs to.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
static int qcomtee_supp_send(struct tee_context *ctx, u32 errno, u32 num_params,
			     struct tee_param *params)
{
	if (!num_params)
		return -EINVAL;

	/* We expect the first parameter to be an OUTPUT + meta parameter. */
	if (params->attr != (TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT |
			     TEE_IOCTL_PARAM_ATTR_META))
		return -EINVAL;

	/* Process TEE parameters. +/-1 to ignore the meta parameter. */
	return qcomtee_user_object_submit(ctx, params + 1, num_params - 1,
					  params->u.value.a, errno);
}

static int qcomtee_open(struct tee_context *ctx)
{
	struct qcomtee_context_data *ctxdata __free(kfree) = NULL;

	ctxdata = kzalloc(sizeof(*ctxdata), GFP_KERNEL);
	if (!ctxdata)
		return -ENOMEM;

	idr_init(&ctxdata->qtee_objects_idr);
	spin_lock_init(&ctxdata->qtee_lock);
	idr_init(&ctxdata->reqs_idr);
	INIT_LIST_HEAD(&ctxdata->reqs_list);
	mutex_init(&ctxdata->reqs_lock);
	init_completion(&ctxdata->req_c);

	ctx->data = no_free_ptr(ctxdata);

	return 0;
}

/* This is called when the user closes the context. */
static void qcomtee_close_context(struct tee_context *ctx)
{
	struct qcomtee_context_data *ctxdata = ctx->data;
	struct qcomtee_object *object;
	int id;

	/* Process QUEUED or PROCESSING requests. */
	qcomtee_requests_destroy(ctxdata);
	/* Release QTEE objects. */
	idr_for_each_entry(&ctxdata->qtee_objects_idr, object, id)
		qcomtee_object_put(object);
}

/* This is called when the final reference to the context goes away. */
static void qcomtee_release(struct tee_context *ctx)
{
	struct qcomtee_context_data *ctxdata = ctx->data;

	idr_destroy(&ctxdata->qtee_objects_idr);
	idr_destroy(&ctxdata->reqs_idr);
	kfree(ctxdata);
}

static void qcomtee_get_version(struct tee_device *teedev,
				struct tee_ioctl_version_data *vers)
{
	struct tee_ioctl_version_data v = {
		.impl_id = TEE_IMPL_ID_QTEE,
		.gen_caps = TEE_GEN_CAP_OBJREF,
	};

	*vers = v;
}

static const struct tee_driver_ops qcomtee_ops = {
	.get_version = qcomtee_get_version,
	.open = qcomtee_open,
	.close_context = qcomtee_close_context,
	.release = qcomtee_release,
	.object_invoke_func = qcomtee_object_invoke,
	.supp_recv = qcomtee_supp_recv,
	.supp_send = qcomtee_supp_send,
};

static const struct tee_desc qcomtee_desc = {
	.name = "qcomtee",
	.ops = &qcomtee_ops,
	.owner = THIS_MODULE,
};

static int qcomtee_probe(struct platform_device *pdev)
{
	struct qcomtee *qcomtee __free(kfree) = NULL;
	struct tee_device *teedev;
	int err;

	qcomtee = kzalloc(sizeof(*qcomtee), GFP_KERNEL);
	if (!qcomtee)
		return -ENOMEM;

	err = qcomtee_release_init();
	if (err)
		return err;

	err = qcomtee_msg_buffers_init();
	if (err)
		goto err_release_destroy;

	teedev = tee_device_alloc(&qcomtee_desc, NULL, NULL, NULL);
	if (IS_ERR(teedev)) {
		err = PTR_ERR(teedev);

		goto err_msg_buffers_destroy;
	}

	qcomtee->teedev = teedev;

	err = tee_device_register(qcomtee->teedev);
	if (err)
		goto err_unreg_teedev;

	platform_set_drvdata(pdev, no_free_ptr(qcomtee));
	return 0;

err_unreg_teedev:
	tee_device_unregister(qcomtee->teedev);
err_msg_buffers_destroy:
	qcomtee_msg_buffers_destroy();
err_release_destroy:
	qcomtee_release_destroy();

	return err;
}

/**
 * qcomtee_remove() - Device Removal Routine.
 * @pdev: platform device information struct.
 *
 * It is called by the platform subsystem to alert the driver that it should
 * release the device.
 */
static void qcomtee_remove(struct platform_device *pdev)
{
	struct qcomtee *qcomtee = platform_get_drvdata(pdev);

	tee_device_unregister(qcomtee->teedev);

	/*
	 * QTEE does not provide an API to inform it about a callback object
	 * going away. However, when releasing QTEE objects, any callback object
	 * sent to QTEE previously would be released by QTEE as part of the
	 * object release.
	 *
	 * Wait for RELEASE operations to be processed for QTEE objects.
	 */
	qcomtee_release_destroy();

	qcomtee_msg_buffers_destroy();
	kfree(qcomtee);
}

static const struct platform_device_id qcomtee_ids[] = { { "qcomtee", 0 }, {} };
MODULE_DEVICE_TABLE(platform, qcomtee_ids);

static struct platform_driver qcomtee_platform_driver = {
	.probe = qcomtee_probe,
	.remove = qcomtee_remove,
	.driver = {
		.name = "qcomtee",
	},
	.id_table = qcomtee_ids,
};

module_platform_driver(qcomtee_platform_driver);

MODULE_AUTHOR("Qualcomm");
MODULE_DESCRIPTION("QTEE driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
