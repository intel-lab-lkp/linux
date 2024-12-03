// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/tee.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/firmware/qcom/qcom_scm.h>

#include "qcomtee_private.h"

/**
 * enum qcom_tee_object_host - Object host where it is managed.
 * @QCOM_TEE_OBJECT_HOST_USER: objects in userspace.
 * @QCOM_TEE_OBJECT_HOST_TEE: objects in QTEE.
 *
 * We refer to object hosted in userspace as 'Local Object' and objects hosted
 * in QTEE as 'Remote Object'.
 */
enum qcom_tee_object_host {
	QCOM_TEE_OBJECT_HOST_USER,	/* Object that is managed in userspace. */
	QCOM_TEE_OBJECT_HOST_TEE,	/* Object that is managed in QTEE. */
};

/* Read object ID host information. */
static enum qcom_tee_object_host qcom_tee_object_host(struct tee_param *param)
{
	if (param->u.objref.flags & QCOM_TEE_OBJREF_FLAG_USER)
		return QCOM_TEE_OBJECT_HOST_USER;
	return QCOM_TEE_OBJECT_HOST_TEE;
}

/* qcom_tee_context_add_qtee_object() - Add a QTEE object to the context.
 * @param: TEE parameter represents @object.
 * @object: QTEE object.
 * @ctx: context to add the object.
 *
 * It assumes @object is %QCOM_TEE_OBJECT_TYPE_TEE and caller has already issued
 * qcom_tee_object_get() for @object.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcom_tee_context_add_qtee_object(struct tee_param *param, struct qcom_tee_object *object,
				     struct qcom_tee_context *ctx)
{
	int ret;

	guard(mutex)(&ctx->lock);
	ret = idr_alloc(&ctx->qtee_objects_idr, object, 0, 0, GFP_KERNEL);
	if (ret < 0)
		return ret;

	param->u.objref.id = ret;
	/* QTEE Object: !QCOM_TEE_OBJREF_FLAG_USER. */
	param->u.objref.flags = 0;

	return 0;
}

static int find_qtee_object(struct qcom_tee_object **object, unsigned long id,
			    struct qcom_tee_context *ctx)
{
	int err = 0;

	guard(rcu)();
	/* Object release is RCU protected. */
	*object = idr_find(&ctx->qtee_objects_idr, id);
	if (!qcom_tee_object_get(*object))
		err = -EINVAL;

	return err;
}

static void del_qtee_object(unsigned long id, struct qcom_tee_context *ctx)
{
	struct qcom_tee_object *object;

	scoped_guard(mutex, &ctx->lock)
		object = idr_remove(&ctx->qtee_objects_idr, id);
	qcom_tee_object_put(object);
}

/* Get the QTEE object added with qcom_tee_context_add_qtee_object(). */
int qcom_tee_context_find_qtee_object(struct qcom_tee_object **object, struct tee_param *param,
				      struct qcom_tee_context *ctx)
{
	/* 'qtee_objects_idr' stores QTEE objects only. */
	if (qcom_tee_object_host(param) != QCOM_TEE_OBJECT_HOST_TEE)
		return -EINVAL;
	return find_qtee_object(object, param->u.objref.id, ctx);
}

/**
 * qcom_tee_context_del_qtee_object() - Delete a QTEE object from the context.
 * @param: TEE parameter represents @object.
 * @ctx: context to delete the object.
 *
 * @param returned by qcom_tee_context_add_qtee_object().
 */
void qcom_tee_context_del_qtee_object(struct tee_param *param, struct qcom_tee_context *ctx)
{
	/* 'qtee_objects_idr' stores QTEE objects only. */
	if (qcom_tee_object_host(param) == QCOM_TEE_OBJECT_HOST_TEE)
		del_qtee_object(param->u.objref.id, ctx);
}

/**
 * qcom_tee_objref_to_arg() - Convert OBJREF parameter to QTEE argument in a context.
 * @arg: QTEE argument.
 * @param: TEE parameter.
 * @ctx: context in which the conversion should happen.
 *
 * It assumes @param is OBJREF.
 * It does not set @arg.type; caller should initialize it to a correct
 * &enum qcom_tee_arg_type value.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcom_tee_objref_to_arg(struct qcom_tee_arg *arg, struct tee_param *param,
			   struct qcom_tee_context *ctx)
{
	struct qcom_tee_object *object;
	int err;

	if (arg->type != QCOM_TEE_ARG_TYPE_IO &&
	    arg->type != QCOM_TEE_ARG_TYPE_OO)
		return -EINVAL;

	/* It is a NULL object?! */
	if (param->u.objref.id == TEE_OBJREF_NULL) {
		arg->o = NULL_QCOM_TEE_OBJECT;

		return 0;
	}

	switch (qcom_tee_object_host(param)) {
	case QCOM_TEE_OBJECT_HOST_USER:
		err = qcom_tee_user_param_to_object(&object, param, ctx);
		if (err)
			break;

		/* Keep a copy for driver as QTEE may release it (e.g. using async msg). */
		qcom_tee_object_get(object);

		break;
	case QCOM_TEE_OBJECT_HOST_TEE:
		err = qcom_tee_context_find_qtee_object(&object, param, ctx);

		break;
	}

	arg->o = err ? NULL_QCOM_TEE_OBJECT : object;

	return err;
}

/**
 * qcom_tee_objref_from_arg() - Convert QTEE argument to OBJREF param in a context.
 * @param: TEE parameter.
 * @arg: QTEE argument.
 * @ctx: context in which the conversion should happen.
 *
 * It assumes @arg is of %QCOM_TEE_ARG_TYPE_IO or %QCOM_TEE_ARG_TYPE_OO.
 * It does not set @param.attr; caller should initialize it to a correct OBJREF type.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcom_tee_objref_from_arg(struct tee_param *param, struct qcom_tee_arg *arg,
			     struct qcom_tee_context *ctx)
{
	struct qcom_tee_object *object;
	int err;

	/* param should be of OBJREF. */
	if (param->attr != TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT &&
	    param->attr != TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT &&
	    param->attr != TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INOUT)
		return -EINVAL;

	object = arg->o;

	switch (typeof_qcom_tee_object(object)) {
	case QCOM_TEE_OBJECT_TYPE_NULL:
		param->u.objref.id = TEE_OBJREF_NULL;
		err = 0;

		break;
	case QCOM_TEE_OBJECT_TYPE_CB_OBJECT:
		err = qcom_tee_user_param_from_object(param, object, ctx);

		break;
	case QCOM_TEE_OBJECT_TYPE_TEE:
		err = qcom_tee_context_add_qtee_object(param, object, ctx);

		break;
	case QCOM_TEE_OBJECT_TYPE_ROOT:
	default:
		return -EINVAL;
	}

	return err;
}

/**
 * qcom_tee_params_to_args() - Convert TEE parameters to QTEE arguments in a context.
 * @u: QTEE arguments.
 * @params: TEE parameters.
 * @num_params: number of elements in the parameter array.
 * @ctx: context in which the conversion should happen.
 *
 * It assumes @u has at least @num_param + 1 entries and has been initialized
 * with %QCOM_TEE_ARG_TYPE_INV as &struct qcom_tee_arg.type.
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_params_to_args(struct qcom_tee_arg *u,
				   struct tee_param *params, int num_params,
				   struct qcom_tee_context *ctx)
{
	struct qcom_tee_object *object;
	int i;

	for (i = 0; i < num_params; i++) {
		switch (params[i].attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_OUTPUT:
			u[i].flags = QCOM_TEE_ARG_FLAGS_UADDR;
			u[i].b.uaddr = params[i].u.membuf.uaddr;
			u[i].b.size = params[i].u.membuf.size;

			if (params[i].attr == TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_INPUT)
				u[i].type = QCOM_TEE_ARG_TYPE_IB;
			else /* TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_OUTPUT */
				u[i].type = QCOM_TEE_ARG_TYPE_OB;

			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT:
			u[i].type = QCOM_TEE_ARG_TYPE_IO;
			if (qcom_tee_objref_to_arg(&u[i], &params[i], ctx))
				goto out_failed;

			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT:
			u[i].type = QCOM_TEE_ARG_TYPE_OO;
			u[i].o = NULL_QCOM_TEE_OBJECT;
			break;
		default: /* Unsupported TEE parameters. */
			goto out_failed;
		}
	}

	return 0;

out_failed:

	/* On ERROR: */
	/* Put IO objects processed so far. */
	for (--i; i >= 0; i--) {
		if (u[i].type != QCOM_TEE_ARG_TYPE_IO)
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
 * qcom_tee_params_from_args() - Convert QTEE arguments to TEE parameters in a context.
 * @params: TEE parameters.
 * @u: QTEE arguments.
 * @num_params: number of elements in the parameter array.
 * @ctx: context in which the conversion should happen.
 *
 * @u should have been already initialized by qcom_tee_params_to_args().
 * This also represents end of a QTEE invocation that started with qcom_tee_params_to_args()
 * by releasing %QCOM_TEE_ARG_TYPE_IO objects.
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_params_from_args(struct tee_param *params,
				     struct qcom_tee_arg *u, int num_params,
				     struct qcom_tee_context *ctx)
{
	int i, np;

	for (np = 0; u[np].type; np++) {
		enum qcom_tee_arg_type at = u[np].type;

		if (at == QCOM_TEE_ARG_TYPE_OB) {
			/* TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_OUTPUT */
			params[np].u.value.b = u[np].b.size;

		} else if (at == QCOM_TEE_ARG_TYPE_IO) {
			/* IEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT */
			qcom_tee_object_put(u[np].o);

		} else if (at == QCOM_TEE_ARG_TYPE_OO) {
			/* TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT */
			if (qcom_tee_objref_from_arg(&params[np], &u[np], ctx))
				goto out_failed;
		}
	}

	return 0;

out_failed:

	/* On ERROR: */
	/*  - Release params associated to QTEE objects in this context so far. */
	for (i = 0; i < np; i++) {
		if (params[i].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT)
			qcom_tee_context_del_qtee_object(&params[i], ctx);
	}
	/*  - Release any IO and OO objects not processed so far. */
	while (u[i].type) {
		if (u[i].type == QCOM_TEE_ARG_TYPE_OO ||
		    u[i].type == QCOM_TEE_ARG_TYPE_IO)
			qcom_tee_object_put(u[i++].o);
	}

	return -EINVAL;
}

/* TEE Device Ops. */

static int qcom_tee_params_check(struct tee_param *params, int num_params)
{
	int io = 0, oo = 0, ib = 0, ob = 0;
	int i;

	/* QTEE accepts 64 arguments. */
	if (num_params > QCOM_TEE_ARGS_MAX)
		return -EINVAL;

	/* Supported parameter types. */
	for (i = 0; i < num_params; i++) {
		switch (params[i].attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_INPUT:
			ib++; break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_OUTPUT:
			ob++; break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT:
			io++; break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT:
			oo++; break;
		default:
			return -EINVAL;
		}
	}

	/*  QTEE accepts 16 arguments of each supported types. */
	if (io > QCOM_TEE_ARGS_PER_TYPE || oo > QCOM_TEE_ARGS_PER_TYPE ||
	    ib > QCOM_TEE_ARGS_PER_TYPE || ob > QCOM_TEE_ARGS_PER_TYPE)
		return -EINVAL;

	return 0;
}

/* Check if user issued a permitted operation on ROOT_QCOM_TEE_OBJECT from userspace. */
static int qcom_tee_root_object_check(u32 op, struct tee_param *params, int num_params)
{
	/* Some privileged operations recognized by QTEE. */
	if (op == 4 || op == 8 || op == 9)
		return -EINVAL;

	/* OP 5 is to register with QTEE by passing credential object as input OBJREF. */
	/* TEE_OBJREF_NULL as credential object represents a privileged client for QTEE,
	 * only kernel can pass TEE_OBJREF_NULL.
	 */
	if (op == 5) {
		if (num_params != 2)
			return -EINVAL;

		if (params[0].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT &&
		    params[1].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT) {
			if (params[0].u.objref.id == TEE_OBJREF_NULL)
				return -EINVAL;

		} else if (params[0].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT &&
			   params[1].attr == TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT) {
			if (params[1].u.objref.id == TEE_OBJREF_NULL)
				return -EINVAL;
		}
	}

	return 0;
}

/**
 * qcom_tee_object_invoke() - Invoke a QTEE object.
 * @tee_ctx: TEE context.
 * @arg: ioctl arguments.
 * @params: parameters for the object.
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_object_invoke(struct tee_context *tee_ctx,
				  struct tee_ioctl_object_invoke_arg *arg,
				  struct tee_param *params)
{
	struct qcom_tee_object_invoke_ctx *oic __free(kfree) = NULL;
	struct qcom_tee_context *ctx = tee_ctx->data;
	struct qcom_tee_arg *u __free(kfree) = NULL;
	struct qcom_tee_object *object;
	int i, ret, result;

	if (qcom_tee_params_check(params, arg->num_params))
		return -EINVAL;

	/* Handle OBJREF reserved operations. */
	if (arg->op == QCOM_TEE_OBJREF_OP_RELEASE) {
		del_qtee_object(arg->object, ctx);

		return 0;
	}

	/* Unsupported reserved operation. */
	if (arg->op > QCOM_TEE_OBJREF_OP_MIN)
		return -EINVAL;

	oic = kzalloc(sizeof(*oic), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	/* +1 for ending QCOM_TEE_ARG_TYPE_INV. */
	u = kzalloc(sizeof(*u) * (arg->num_params + 1), GFP_KERNEL);
	if (!u)
		return -ENOMEM;

	if (arg->object == TEE_OBJREF_NULL &&
	    !qcom_tee_root_object_check(arg->op, params, arg->num_params)) {
		/* Use ROOT if NULL is invoked. */
		object = ROOT_QCOM_TEE_OBJECT;
	} else {
		/* Get object being invoked. */
		ret = find_qtee_object(&object, arg->object, ctx);
		if (ret)
			return ret;
	}

	ret = qcom_tee_params_to_args(u, params, arg->num_params, ctx);
	if (ret)
		goto out;

	ret = qcom_tee_object_do_invoke(oic, object, arg->op, u, &result);
	if (!ret) {
		if (!result) {
			/* Only parse QTEE response on SUCCESS. */
			ret = qcom_tee_params_from_args(params, u, arg->num_params, ctx);
		} else {
			/* Put driver's IO objects copy; get in qcom_tee_params_to_args(). */
			qcom_tee_arg_for_each_input_object(i, u)
				qcom_tee_object_put(u[i++].o);
		}
	} else if (ret != -EAGAIN && ret != -ENODEV) {
		/* Unable to initiate a QTEE invocation; cleanup qcom_tee_params_to_args(). */
		qcom_tee_arg_for_each_input_object(i, u) {
			qcom_tee_user_object_set_notify(u[i].o, false);
			if (typeof_qcom_tee_object(u[i].o) == QCOM_TEE_OBJECT_TYPE_CB_OBJECT)
				qcom_tee_object_put(u[i].o);
			qcom_tee_object_put(u[i].o);
		}
	}

	arg->ret = result;
out:
	qcom_tee_object_put(object);

	return ret;
}

/**
 * qcom_tee_supp_recv() - Pick a request for the supplicant.
 * @tee_ctx: TEE context.
 * @op: requested operation on object.
 * @num_params: number of elements in the parameter array, updated with number used.
 * @params: parameters for @op.
 *
 * The first parameter is a %TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT meta parameter.
 * On input, it provides a user buffer. This buffer is used for parameters of type
 * %TEE_IOCTL_PARAM_ATTR_TYPE_MEMBUF_INPUT in qcom_tee_cb_params_from_args().
 * On output, object id and request id are stored in the meta parameter.
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_supp_recv(struct tee_context *tee_ctx, u32 *op, u32 *num_params,
			      struct tee_param *params)
{
	struct qcom_tee_user_object_request_data data;
	struct qcom_tee_context *ctx = tee_ctx->data;
	void __user *uaddr;
	size_t ubuf_size;
	int i, ret;

	if (!*num_params)
		return -EINVAL;

	/* We expect the first parameter to be an INOUT + meta parameter. */
	if (params->attr != (TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT | TEE_IOCTL_PARAM_ATTR_META))
		return -EINVAL;

	/* Others parameters are none. */
	for (i = 1; i < *num_params; i++)
		if (params[i].attr)
			return -EINVAL;

	if (IS_ALIGNED(params->u.value.a, 8))
		return -EINVAL;

	/* User buffer and size from meta parameter. */
	uaddr = u64_to_user_ptr(params->u.value.a);
	ubuf_size = params->u.value.b;
	/* Process TEE parameters. +/-1 to ignore meta parameter. */
	ret = qcom_tee_user_object_pop(ctx, params + 1, *num_params - 1, uaddr, ubuf_size, &data);
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
 * qcom_tee_supp_send() - Pick a request for the supplicant.
 * @tee_ctx: TEE context.
 * @ret: return value of the request.
 * @num_params: number of elements in the parameter array.
 * @params: returned parameters.
 *
 * The first parameter is a %TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT meta parameter.
 * It specifies the request id this response is belong to.
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_supp_send(struct tee_context *tee_ctx, u32 errno, u32 num_params,
			      struct tee_param *params)
{
	struct qcom_tee_context *ctx = tee_ctx->data;
	int id;

	if (!num_params)
		return -EINVAL;

	/* We expect the first parameter to be an INPUT + meta parameter. */
	if (params->attr != (TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT | TEE_IOCTL_PARAM_ATTR_META))
		return -EINVAL;

	/* Get the req_id of response. */
	id = params->u.value.a;

	/* Process TEE parameters. +/-1 to ignore meta parameter. */
	return qcom_tee_user_object_submit(ctx, params + 1, num_params - 1, id, errno);
}

static int qcom_tee_open(struct tee_context *tee_context)
{
	struct qcom_tee_context *ctx __free(kfree) = NULL;
	int err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	err = init_srcu_struct(&ctx->req_srcu);
	if (err)
		return err;

	ctx->tee_context = tee_context;
	idr_init(&ctx->qtee_objects_idr);
	idr_init(&ctx->reqs_idr);
	mutex_init(&ctx->lock);
	init_completion(&ctx->req_c);
	kref_init(&ctx->ref_cnt);

	tee_context->data = no_free_ptr(ctx);

	return 0;
}

static void qcom_tee_release(struct tee_context *tee_context)
{
	struct qcom_tee_context *ctx = tee_context->data;
	struct qcom_tee_object *object;
	int id;

	/* Process QUEUED or PROCESSING requests. */
	qcom_tee_requests_destroy(ctx);

	/* Release QTEE objects. */
	idr_for_each_entry(&ctx->qtee_objects_idr, object, id)
		qcom_tee_object_put(object);

	/* Put the context; wait for all user objects to go away. */
	kref_put(&ctx->ref_cnt, __qcom_tee_context_destroy);
}

void __qcom_tee_context_destroy(struct kref *ref_cnt)
{
	struct qcom_tee_context *ctx = container_of(ref_cnt, struct qcom_tee_context, ref_cnt);

	idr_destroy(&ctx->qtee_objects_idr);
	idr_destroy(&ctx->reqs_idr);
	cleanup_srcu_struct(&ctx->req_srcu);
	kfree(ctx);
}

static void qcom_tee_get_version(struct tee_device *teedev, struct tee_ioctl_version_data *vers)
{
	struct tee_ioctl_version_data v = {
		.impl_id = TEE_IMPL_ID_QTEE,
		.gen_caps = TEE_GEN_CAP_OBJREF,
	};

	*vers = v;
}

static const struct tee_driver_ops qcom_tee_ops = {
	.get_version = qcom_tee_get_version,
	.open = qcom_tee_open,
	.release = qcom_tee_release,
	.object_invoke_func = qcom_tee_object_invoke,
	.supp_recv = qcom_tee_supp_recv,
	.supp_send = qcom_tee_supp_send,
};

static const struct tee_desc qcom_tee_desc = {
	.name = "qcom_tee",
	.ops = &qcom_tee_ops,
	.owner = THIS_MODULE,
};

static int qcom_tee_probe(struct platform_device *pdev)
{
	struct tee_device *teedev;
	int err;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	teedev = tee_device_alloc(&qcom_tee_desc, NULL, NULL, NULL);
	if (IS_ERR(teedev))
		return PTR_ERR(teedev);

	err = tee_device_register(teedev);
	if (err)
		goto err_unreg_teedev;

	platform_set_drvdata(pdev, teedev);
	return 0;

err_unreg_teedev:
	tee_device_unregister(teedev);

	return err;
}

static void qcom_tee_remove(struct platform_device *pdev)
{
	struct tee_device *teedev = platform_get_drvdata(pdev);

	/* Keep a copy, tee_device_unregister() sets it to NULL. */
	struct tee_shm_pool *pool = teedev->pool;

	/* Wait for users to go away. */
	tee_device_unregister(teedev);
	tee_shm_pool_free(pool);
}

static const struct of_device_id qcom_tee_dt_match[] = {
	{ .compatible = "qcom,tee" },
	{},
};
MODULE_DEVICE_TABLE(of, qcom_tee_dt_match);

static struct platform_driver qcom_tee_platform_driver = {
	.probe = qcom_tee_probe,
	.remove = qcom_tee_remove,
	.driver = {
		.name = "qcom_tee",
		.of_match_table = qcom_tee_dt_match,
	},
};

int qcom_tee_driver_register(void)
{
	return platform_driver_register(&qcom_tee_platform_driver);
}

void qcom_tee_driver_unregister(void)
{
	platform_driver_unregister(&qcom_tee_platform_driver);
}
