// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "qcomtee_msg.h"
#include "qcomtee_private.h"

/* Static instance of object represents QTEE root object. */
struct qcom_tee_object qcom_tee_object_root = {
	.name = "root",
	.object_type = QCOM_TEE_OBJECT_TYPE_ROOT,
	.info.qtee_id = QCOM_TEE_MSG_OBJECT_ROOT,
};
EXPORT_SYMBOL_GPL(qcom_tee_object_root);

/* Next argument of type @type after index @i. */
int qcom_tee_next_arg_type(struct qcom_tee_arg *u, int i, enum qcom_tee_arg_type type)
{
	while (u[i].type != QCOM_TEE_ARG_TYPE_INV && u[i].type != type)
		i++;
	return i;
}

/* QTEE expects IDs with QCOM_TEE_MSG_OBJECT_NS_BIT set for object of
 * QCOM_TEE_OBJECT_TYPE_CB_OBJECT type. The first ID with QCOM_TEE_MSG_OBJECT_NS_BIT set is
 * reserved for primordial object.
 */
#define QCOM_TEE_OBJECT_PRIMORDIAL	(QCOM_TEE_MSG_OBJECT_NS_BIT)
#define QCOM_TEE_OBJECT_ID_START	(QCOM_TEE_OBJECT_PRIMORDIAL + 1)
#define QCOM_TEE_OBJECT_ID_END		(UINT_MAX)

#define QCOM_TEE_OBJECT_SET(p, type, ...) __QCOM_TEE_OBJECT_SET(p, type, ##__VA_ARGS__, 0UL)
#define __QCOM_TEE_OBJECT_SET(p, type, optr, ...) do { \
		(p)->object_type = (type); \
		(p)->info.qtee_id = (unsigned long)(optr); \
	} while (0)

static struct qcom_tee_object *qcom_tee_object_alloc(void)
{
	struct qcom_tee_object *object;

	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (object) {
		QCOM_TEE_OBJECT_SET(object, QCOM_TEE_OBJECT_TYPE_NULL);
		kref_init(&object->refcount);
	}

	return object;
}

void qcom_tee_object_free(struct qcom_tee_object *object)
{
	kfree(object->name);
	kfree(object);
}

static void qcom_tee_object_release(struct kref *refcount)
{
	struct qcom_tee_object *object;
	struct module *owner;
	const char *name;

	object = container_of(refcount, struct qcom_tee_object, refcount);

	synchronize_rcu();

	switch (typeof_qcom_tee_object(object)) {
	case QCOM_TEE_OBJECT_TYPE_TEE:
		qcom_tee_release_tee_object(object);

		break;
	case QCOM_TEE_OBJECT_TYPE_CB_OBJECT:
		/* Copy, as after release we should not access object. */
		name = object->name;
		owner = object->owner;

		if (object->ops->release)
			object->ops->release(object);

		module_put(owner);
		kfree_const(name);

		break;
	case QCOM_TEE_OBJECT_TYPE_ROOT:
	case QCOM_TEE_OBJECT_TYPE_NULL:
	default:
		break;
	}
}

/**
 * qcom_tee_object_get() - Increase object's refcount.
 * @object: object to increase the refcount
 */
int qcom_tee_object_get(struct qcom_tee_object *object)
{
	if (object != NULL_QCOM_TEE_OBJECT &&
	    object != ROOT_QCOM_TEE_OBJECT)
		return kref_get_unless_zero(&object->refcount);

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_tee_object_get);

/**
 * qcom_tee_object_put() - Decrease object's refcount
 * @object: object to decrease the refcount
 */
void qcom_tee_object_put(struct qcom_tee_object *object)
{
	if (object != &qcom_tee_primordial_object &&
	    object != NULL_QCOM_TEE_OBJECT &&
	    object != ROOT_QCOM_TEE_OBJECT)
		kref_put(&object->refcount, qcom_tee_object_release);
}
EXPORT_SYMBOL_GPL(qcom_tee_object_put);

/* ''Local Object Table''. */
/* Object from kernel that are exported to QTEE are assigned an id and stored in
 * xa_qcom_local_objects (kernel object table). QTEE uses this id to reference the
 * object using qcom_tee_local_object_get.
 */
static DEFINE_XARRAY_ALLOC(xa_qcom_local_objects);

static int qcom_tee_idx_alloc(u32 *idx, struct qcom_tee_object *object)
{
	static u32 xa_last_id = QCOM_TEE_OBJECT_ID_START;

	/* Every id allocated here, has QCOM_TEE_MSG_OBJECT_NS_BIT set. */
	return xa_alloc_cyclic(&xa_qcom_local_objects, idx, object,
		XA_LIMIT(QCOM_TEE_OBJECT_ID_START, QCOM_TEE_OBJECT_ID_END),
			&xa_last_id, GFP_KERNEL);
}

struct qcom_tee_object *qcom_tee_idx_erase(u32 idx)
{
	if (idx < QCOM_TEE_OBJECT_ID_START || idx > QCOM_TEE_OBJECT_ID_END)
		return NULL_QCOM_TEE_OBJECT;

	return xa_erase(&xa_qcom_local_objects, idx);
}

/**
 * qcom_tee_object_id_get() - Get an id for an object to sent to QTEE.
 * @object: object to get its id.
 * @object_id: object id.
 *
 * For object hosted in REE, they are added to object table, and the idx in the
 * object table is used as id. For object hosted in QTEE, use the QTEE id stored in
 * @object. This is called on a path to QTEE to construct a message, see
 * qcom_tee_prepare_msg() and qcom_tee_update_msg().
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_object_id_get(struct qcom_tee_object *object, unsigned int *object_id)
{
	u32 idx;

	switch (typeof_qcom_tee_object(object)) {
	case QCOM_TEE_OBJECT_TYPE_CB_OBJECT:
		if (qcom_tee_idx_alloc(&idx, object) < 0)
			return -ENOSPC;

		*object_id = idx;

		break;
	case QCOM_TEE_OBJECT_TYPE_ROOT:
	case QCOM_TEE_OBJECT_TYPE_TEE:
		*object_id = object->info.qtee_id;

		break;
	case QCOM_TEE_OBJECT_TYPE_NULL:
		*object_id = QCOM_TEE_MSG_OBJECT_NULL;

		break;
	}

	return 0;
}

/* Release object id assigned in qcom_tee_object_id_get. */
static void qcom_tee_object_id_put(unsigned int object_id)
{
	qcom_tee_idx_erase(object_id);
}

/**
 * qcom_tee_local_object_get() - Get an object in REE referenced by the id.
 * @object_id: object id.
 *
 * It is called on behalf of QTEE to obtain instance of object for an id. It is
 * called on a path from QTEE to construct an argument of &struct qcom_tee_arg,
 * see qcom_tee_update_args() and qcom_tee_prepare_args().
 *
 * It increases the object's refcount on success.
 *
 * Return: On error returns %NULL_QCOM_TEE_OBJECT. On success, the object.
 */
static struct qcom_tee_object *qcom_tee_local_object_get(unsigned int object_id)
{
	struct qcom_tee_object *object;

	if (object_id == QCOM_TEE_OBJECT_PRIMORDIAL)
		return &qcom_tee_primordial_object;

	/* We trust QTEE does not mess the refcounts.
	 * It does not issue RELEASE request and qcom_tee_object_get(), simultaneously.
	 */

	object = xa_load(&xa_qcom_local_objects, object_id);

	qcom_tee_object_get(object);

	return object;
}

/**
 * __qcom_tee_object_user_init() - Initialize an object for user.
 * @object: object to initialize.
 * @ot: type of object as &enum qcom_tee_object_type.
 * @ops: instance of callbacks.
 * @fmt: name assigned to the object.
 *
 * Return: On success return 0 or <0 on failure.
 */
int __qcom_tee_object_user_init(struct qcom_tee_object *object, enum qcom_tee_object_type ot,
				struct qcom_tee_object_operations *ops, struct module *owner,
				const char *fmt, ...)
{
	va_list ap;
	int ret;

	kref_init(&object->refcount);
	QCOM_TEE_OBJECT_SET(object, QCOM_TEE_OBJECT_TYPE_NULL);

	va_start(ap, fmt);
	switch (ot) {
	case QCOM_TEE_OBJECT_TYPE_NULL:
		ret = 0;

		break;
	case QCOM_TEE_OBJECT_TYPE_CB_OBJECT:
		object->ops = ops;
		if (!object->ops->dispatch)
			return -EINVAL;

		object->owner = owner;
		if (!try_module_get(object->owner))
			return -EINVAL;

		/* If failed, "no-name"; it is not really a reason to fail here. */
		object->name = kvasprintf_const(GFP_KERNEL, fmt, ap);
		QCOM_TEE_OBJECT_SET(object, QCOM_TEE_OBJECT_TYPE_CB_OBJECT);

		ret = 0;
		break;
	case QCOM_TEE_OBJECT_TYPE_ROOT:
	case QCOM_TEE_OBJECT_TYPE_TEE:
	default:
		ret = -EINVAL;
	}
	va_end(ap);

	return ret;
}
EXPORT_SYMBOL_GPL(__qcom_tee_object_user_init);

/**
 * qcom_tee_object_type() - Returns type of object represented by an object id.
 * @object_id: object id for the object.
 *
 * This is similar to typeof_qcom_tee_object() but instead of receiving object
 * as argument it receives object id. It is used internally on return path
 * from QTEE.
 *
 * Return: Returns type of object referenced by @object_id.
 */
static enum qcom_tee_object_type qcom_tee_object_type(unsigned int object_id)
{
	if (object_id == QCOM_TEE_MSG_OBJECT_NULL)
		return QCOM_TEE_OBJECT_TYPE_NULL;

	if (object_id & QCOM_TEE_MSG_OBJECT_NS_BIT)
		return QCOM_TEE_OBJECT_TYPE_CB_OBJECT;

	return QCOM_TEE_OBJECT_TYPE_TEE;
}

/**
 * qcom_tee_object_init() - Initialize an object for QTEE.
 * @object: return object
 * @object_id: object id received form QTEE
 *
 * Return: On success return 0 or <0 on failure.
 */
static int qcom_tee_object_init(struct qcom_tee_object **object, unsigned int object_id)
{
	struct qcom_tee_object *qto;
	int ret = 0;

	switch (qcom_tee_object_type(object_id)) {
	case QCOM_TEE_OBJECT_TYPE_NULL:
		*object = NULL_QCOM_TEE_OBJECT;

		break;
	case QCOM_TEE_OBJECT_TYPE_CB_OBJECT:
		qto = qcom_tee_local_object_get(object_id);
		if (qto != NULL_QCOM_TEE_OBJECT)
			*object = qto;
		else
			ret = -EINVAL;

		break;
	case QCOM_TEE_OBJECT_TYPE_TEE:
		qto = qcom_tee_object_alloc();
		if (qto) {
			/* If failed, "no-name"; it is not really a reason to fail here. */
			qto->name = kasprintf(GFP_KERNEL, "qcom_tee-%u", object_id);
			QCOM_TEE_OBJECT_SET(qto, QCOM_TEE_OBJECT_TYPE_TEE, object_id);

			*object = qto;
		} else {
			ret = -ENOMEM;
		}

		break;
	default:

		break;
	}

	if (ret)
		*object = NULL_QCOM_TEE_OBJECT;

	return ret;
}

/* Marshaling API. */
/* qcom_tee_prepare_msg  - Prepares inbound buffer for sending to QTEE
 * qcom_tee_update_args  - Parses QTEE response in inbound buffer
 * qcom_tee_prepare_args - Parses QTEE request from outbound buffer
 * qcom_tee_update_msg   - Updates outbound buffer with response for QTEE request
 */

static int qcom_tee_prepare_msg(struct qcom_tee_object_invoke_ctx *oic,
				struct qcom_tee_object *object, u32 op,	struct qcom_tee_arg *u)
{
	struct qcom_tee_msg_object_invoke *msg;
	unsigned int object_id;
	int ib, ob, io, oo;
	size_t off;
	int i;

	/* Use input message buffer in 'oic'. */
	msg = (struct qcom_tee_msg_object_invoke *)oic->in_msg.addr;

	/* Start offset in a message for buffer arguments. */
	off = qcom_tee_msg_buffer_args(struct qcom_tee_msg_object_invoke, qcom_tee_args_len(u));

	/* Get id of object being invoked. */
	if (qcom_tee_object_id_get(object, &object_id))
		return -ENOSPC;

	ib = 0;
	qcom_tee_arg_for_each_input_buffer(i, u) {
		void *ptr;

		/* qcom_tee_msg_buffers_alloc() already checked overflow in message! */
		msg->args[ib].b.offset = off;
		msg->args[ib].b.size = u[i].b.size;

		ptr = qcom_tee_msg_offset_to_ptr(msg, off);
		if (!(u[i].flags & QCOM_TEE_ARG_FLAGS_UADDR))
			memcpy(ptr, u[i].b.addr, u[i].b.size);
		else if (copy_from_user(ptr, u[i].b.uaddr, u[i].b.size))
			return -EINVAL;

		off += qcom_tee_msg_offset_align(u[i].b.size);
		ib++;
	}

	ob = ib;
	qcom_tee_arg_for_each_output_buffer(i, u) {
		/* qcom_tee_msg_buffers_alloc() already checked overflow in message! */
		msg->args[ob].b.offset = off;
		msg->args[ob].b.size = u[i].b.size;

		off += qcom_tee_msg_offset_align(u[i].b.size);
		ob++;
	}

	io = ob;
	qcom_tee_arg_for_each_input_object(i, u) {
		if (qcom_tee_object_id_get(u[i].o, &msg->args[io].o)) {
			/* Unable to qcom_tee_object_id_get; put whatever we got. */
			qcom_tee_object_id_put(object_id);
			for (--io; io >= ob; io--)
				qcom_tee_object_id_put(msg->args[io].o);

			return -ENOSPC;
		}

		io++;
	}

	oo = io;
	qcom_tee_arg_for_each_output_object(i, u)
		oo++;

	/* Set object, operation, and argument counts. */
	qcom_tee_msg_init(msg, object_id, op, ib, ob, io, oo);

	return 0;
}

static int qcom_tee_update_args(struct qcom_tee_arg *u, struct qcom_tee_object_invoke_ctx *oic)
{
	struct qcom_tee_msg_object_invoke *msg;
	int ib, ob, io, oo;
	int i, ret = 0;

	/* Use input message buffer in 'oic'. */
	msg = (struct qcom_tee_msg_object_invoke *)oic->in_msg.addr;

	ib = 0;
	qcom_tee_arg_for_each_input_buffer(i, u)
		ib++;

	ob = ib;
	qcom_tee_arg_for_each_output_buffer(i, u) {
		void *ptr = qcom_tee_msg_offset_to_ptr(msg, msg->args[ob].b.offset);

		if (!(u[i].flags & QCOM_TEE_ARG_FLAGS_UADDR)) {
			memcpy(u[i].b.addr, ptr, msg->args[ob].b.size);
		} else if (copy_to_user(u[i].b.uaddr, ptr, msg->args[ob].b.size)) {
			/* On ERROR, continue to process arguments to get to output object. */
			ret = -EINVAL;
		}

		u[i].b.size = msg->args[ob].b.size;
		ob++;
	}

	io = ob;
	qcom_tee_arg_for_each_input_object(i, u)
		io++;

	oo = io;
	qcom_tee_arg_for_each_output_object(i, u) {
		int err;

		/* On ERROR, continue to process arguments so that we can issue the RELEASE. */
		err = qcom_tee_object_init(&u[i].o, msg->args[oo].o);
		if (err)
			ret = err;

		oo++;
	}

	return ret;
}

static int qcom_tee_prepare_args(struct qcom_tee_object_invoke_ctx *oic)
{
	int i, ret = 0;

	/* Use output message buffer in 'oic'. */
	struct qcom_tee_msg_callback *msg = (struct qcom_tee_msg_callback *)oic->out_msg.addr;

	qcom_tee_msg_for_each_input_buffer(i, msg) {
		oic->u[i].b.addr = qcom_tee_msg_offset_to_ptr(msg, msg->args[i].b.offset);
		oic->u[i].b.size = msg->args[i].b.size;
		oic->u[i].type = QCOM_TEE_ARG_TYPE_IB;
	}

	qcom_tee_msg_for_each_output_buffer(i, msg) {
		oic->u[i].b.addr = qcom_tee_msg_offset_to_ptr(msg, msg->args[i].b.offset);
		oic->u[i].b.size = msg->args[i].b.size;
		oic->u[i].type = QCOM_TEE_ARG_TYPE_OB;
	}

	qcom_tee_msg_for_each_input_object(i, msg) {
		int err;

		/* On ERROR, continue to process arguments so that we can issue the RELEASE. */
		err = qcom_tee_object_init(&oic->u[i].o, msg->args[i].o);
		if (err)
			ret = err;

		oic->u[i].type = QCOM_TEE_ARG_TYPE_IO;
	}

	qcom_tee_msg_for_each_output_object(i, msg)
		oic->u[i].type = QCOM_TEE_ARG_TYPE_OO;

	/* End of Arguments. */
	oic->u[i].type = QCOM_TEE_ARG_TYPE_INV;

	return ret;
}

static int qcom_tee_update_msg(struct qcom_tee_object_invoke_ctx *oic)
{
	int ib, ob, io, oo;
	int i;

	/* Use output message buffer in 'oic'. */
	struct qcom_tee_msg_callback *msg = (struct qcom_tee_msg_callback *)oic->out_msg.addr;

	ib = 0;
	qcom_tee_arg_for_each_input_buffer(i, oic->u)
		ib++;

	ob = ib;
	qcom_tee_arg_for_each_output_buffer(i, oic->u) {
		/* Only reduce size; never increase it. */
		if (msg->args[ob].b.size < oic->u[i].b.size)
			return -EINVAL;

		msg->args[ob].b.size = oic->u[i].b.size;
		ob++;
	}

	io = ob;
	qcom_tee_arg_for_each_input_object(i, oic->u)
		io++;

	oo = io;
	qcom_tee_arg_for_each_output_object(i, oic->u) {
		if (qcom_tee_object_id_get(oic->u[i].o, &msg->args[oo].o)) {
			/* Unable to qcom_tee_object_id_get; put whatever we got. */
			for (--oo; oo >= io; --oo)
				qcom_tee_object_id_put(msg->args[oo].o);

			return -ENOSPC;
		}

		oo++;
	}

	return 0;
}

/**
 * define MAX_BUFFER_SIZE - Maximum size of inbound and outbound buffers.
 *
 * QTEE transport does not impose any restriction on these buffers. However, if size of
 * buffers are larger then %MAX_BUFFER_SIZE, user should probably use some other
 * form of shared memory with QTEE.
 */
#define MAX_BUFFER_SIZE SZ_8K

/* Pool to allocate inbound and outbound buffers. */
static struct qcom_tzmem_pool *tzmem_msg_pool;

static int qcom_tee_msg_buffers_alloc(struct qcom_tee_object_invoke_ctx *oic,
				      struct qcom_tee_arg *u)
{
	size_t size;
	int i;

	/* Start offset in a message for buffer arguments. */
	size = qcom_tee_msg_buffer_args(struct qcom_tee_msg_object_invoke, qcom_tee_args_len(u));
	if (size > MAX_BUFFER_SIZE)
		return -EINVAL;

	/* Add size of IB arguments. */
	qcom_tee_arg_for_each_input_buffer(i, u) {
		size = size_add(size, qcom_tee_msg_offset_align(u[i].b.size));
		if (size > MAX_BUFFER_SIZE)
			return -EINVAL;
	}

	/* Add size of OB arguments. */
	qcom_tee_arg_for_each_output_buffer(i, u) {
		size = size_add(size, qcom_tee_msg_offset_align(u[i].b.size));
		if (size > MAX_BUFFER_SIZE)
			return -EINVAL;
	}

	/* QTEE requires inbound buffer size to be page aligned. */
	size = PAGE_ALIGN(size);

	/* Do allocations. */
	oic->in_msg.size = size;
	oic->in_msg.addr = qcom_tzmem_alloc(tzmem_msg_pool, size, GFP_KERNEL);
	if (!oic->in_msg.addr)
		return -EINVAL;

	oic->out_msg.size = MAX_BUFFER_SIZE;
	oic->out_msg.addr = qcom_tzmem_alloc(tzmem_msg_pool, MAX_BUFFER_SIZE, GFP_KERNEL);
	if (!oic->out_msg.addr) {
		qcom_tzmem_free(oic->in_msg.addr);

		return -EINVAL;
	}

	oic->in_msg_paddr = qcom_tzmem_to_phys(oic->in_msg.addr);
	oic->out_msg_paddr = qcom_tzmem_to_phys(oic->out_msg.addr);

	/* QTEE assume unused buffers are zeroed; Do it now! */
	memzero_explicit(oic->in_msg.addr, oic->in_msg.size);
	memzero_explicit(oic->out_msg.addr, oic->out_msg.size);

	return 0;
}

static void qcom_tee_msg_buffers_free(struct qcom_tee_object_invoke_ctx *oic)
{
	qcom_tzmem_free(oic->in_msg.addr);
	qcom_tzmem_free(oic->out_msg.addr);
}

static int qcom_tee_msg_buffers_init(void)
{
	struct qcom_tzmem_pool_config config = {
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		/* 4M seems enough, it is used for QTEE meg header and qcom_tee_msg_arg array. */
		.max_size = SZ_4M
	};

	tzmem_msg_pool = qcom_tzmem_pool_new(&config);
	if (IS_ERR(tzmem_msg_pool))
		return PTR_ERR(tzmem_msg_pool);

	return 0;
}

static void qcom_tee_msg_buffers_destroy(void)
{
	qcom_tzmem_pool_free(tzmem_msg_pool);
}

/* Invoke a REE object. */
static void qcom_tee_object_invoke(struct qcom_tee_object_invoke_ctx *oic,
				   struct qcom_tee_msg_callback *msg)
{
	int i, errno;
	u32 op;

	/* Get object being invoked. */
	unsigned int object_id = msg->cxt;
	struct qcom_tee_object *object;

	/* QTEE can not invoke NULL object or objects it hosts. */
	if (qcom_tee_object_type(object_id) == QCOM_TEE_OBJECT_TYPE_NULL ||
	    qcom_tee_object_type(object_id) == QCOM_TEE_OBJECT_TYPE_TEE) {
		errno = -EINVAL;
		goto out;
	}

	object = qcom_tee_local_object_get(object_id);
	if (object == NULL_QCOM_TEE_OBJECT) {
		errno = -EINVAL;
		goto out;
	}

	oic->object = object;

	/* Filter bits used by transport. */
	op = msg->op & QCOM_TEE_MSG_OBJECT_OP_MASK;

	switch (op) {
	case QCOM_TEE_MSG_OBJECT_OP_RELEASE:
		qcom_tee_object_id_put(object_id);
		qcom_tee_object_put(object);
		errno = 0;

		break;
	case QCOM_TEE_MSG_OBJECT_OP_RETAIN:
		qcom_tee_object_get(object);
		errno = 0;

		break;
	default:
		errno = qcom_tee_prepare_args(oic);
		if (errno) {
			/* Unable to parse the message. Release any object arrived as input. */
			qcom_tee_arg_for_each_input_buffer(i, oic->u)
				qcom_tee_object_put(oic->u[i].o);

			break;
		}

		errno = object->ops->dispatch(oic, object, op, oic->u);
		if (!errno) {
			/* On SUCCESS, notify object at appropriate time. */
			oic->flags |= QCOM_TEE_OIC_FLAG_NOTIFY;
		}
	}

out:

	oic->errno = errno;
}

/**
 * __qcom_tee_object_do_invoke() - Submit an invocation for an object.
 * @oic: context to use for current invocation.
 * @object: object being invoked.
 * @op: requested operation on object.
 * @u: array of argument for the current invocation.
 * @result: result returned from QTEE.
 *
 * The caller is responsible to keep track of the refcount for each object,
 * including @object. On return, the caller loses the ownership of all input
 * object of type %QCOM_TEE_OBJECT_TYPE_CB_OBJECT.
 *
 * Return: On success return 0. On error returns -EINVAL and -ENOSPC if unable to initiate
 * the invocation, -EAGAIN if invocation failed and user may retry the invocation.
 * Otherwise, -ENODEV on fatal failure.
 */
int __qcom_tee_object_do_invoke(struct qcom_tee_object_invoke_ctx *oic,
				struct qcom_tee_object *object, u32 op,	struct qcom_tee_arg *u,
				int *result)
{
	struct qcom_tee_msg_callback *cb_msg;
	u64 response_type;
	int i, ret, errno;

	ret = qcom_tee_msg_buffers_alloc(oic, u);
	if (ret)
		return ret;

	ret = qcom_tee_prepare_msg(oic, object, op, u);
	if (ret)
		goto out;

	cb_msg = (struct qcom_tee_msg_callback *)oic->out_msg.addr;

	while (1) {
		if (oic->flags & QCOM_TEE_OIC_FLAG_BUSY) {
			errno = oic->errno;
			/* Update output buffer only if result is SUCCESS. */
			if (!errno)
				errno = qcom_tee_update_msg(oic);

			qcom_tee_msg_translate_err(cb_msg, errno);
		}

		/* Invoke remote object. */
		ret = qcom_tee_object_invoke_ctx_invoke(oic, result, &response_type);

		if (oic->flags & QCOM_TEE_OIC_FLAG_BUSY) {
			struct qcom_tee_object *qto = oic->object;

			if (qto) {
				if (oic->flags & QCOM_TEE_OIC_FLAG_NOTIFY) {
					if (qto->ops->notify)
						qto->ops->notify(oic, qto, errno || ret);
				}

				/* Matching get is in qcom_tee_object_invoke. */
				qcom_tee_object_put(qto);
			}

			oic->object = NULL_QCOM_TEE_OBJECT;
			oic->flags &= ~(QCOM_TEE_OIC_FLAG_BUSY | QCOM_TEE_OIC_FLAG_NOTIFY);
		}

		if (ret) {
			if (!(oic->flags & QCOM_TEE_OIC_FLAG_SHARED)) {
				/* Release QCOM_TEE_OBJECT_TYPE_CB_OBJECT input objects. */
				qcom_tee_arg_for_each_input_object(i, u)
					if (typeof_qcom_tee_object(u[i].o) ==
						QCOM_TEE_OBJECT_TYPE_CB_OBJECT)
						qcom_tee_object_put(u[i].o);

				ret = -EAGAIN;
			} else {
				/* On error, there is no clean way to exit. */
				/* For some reason we can not communicate with QTEE, so we can not
				 * notify QTEE about the failure and do further cleanup.
				 */
				ret = -ENODEV;
			}

			goto out;

		} else {
			/* QTEE obtained the ownership of QCOM_TEE_OBJECT_TYPE_CB_OBJECT
			 * input objects in 'u'. On further failure, QTEE is responsible
			 * to release them.
			 */
			oic->flags |= QCOM_TEE_OIC_FLAG_SHARED;
		}

		/* Is it a callback request? */
#define QCOM_TEE_RESULT_INBOUND_REQ_NEEDED 3
		if (response_type != QCOM_TEE_RESULT_INBOUND_REQ_NEEDED) {
			if (!*result) {
				ret = qcom_tee_update_args(u, oic);
				if (ret) {
					qcom_tee_arg_for_each_output_object(i, u)
						qcom_tee_object_put(u[i].o);

					ret = -EAGAIN;
				}
			}

			break;

		} else {
			oic->flags |= QCOM_TEE_OIC_FLAG_BUSY;
			/* Before dispatching the request, handle any pending async requests. */
			qcom_tee_fetch_async_reqs(oic);
			qcom_tee_object_invoke(oic, cb_msg);
		}
	}

	qcom_tee_fetch_async_reqs(oic);

out:
	qcom_tee_msg_buffers_free(oic);

	return ret;
}

int qcom_tee_object_do_invoke(struct qcom_tee_object_invoke_ctx *oic,
			      struct qcom_tee_object *object, u32 op, struct qcom_tee_arg *u,
			      int *result)
{
	/* User can not set bits used by transport. */
	if (op & ~QCOM_TEE_MSG_OBJECT_OP_MASK)
		return -EINVAL;

	/* User can only invoke QTEE hosted objects. */
	if (typeof_qcom_tee_object(object) != QCOM_TEE_OBJECT_TYPE_TEE &&
	    typeof_qcom_tee_object(object) != QCOM_TEE_OBJECT_TYPE_ROOT)
		return -EINVAL;

	/* User can not issue reserved operations to QTEE. */
	if (op == QCOM_TEE_MSG_OBJECT_OP_RELEASE || op == QCOM_TEE_MSG_OBJECT_OP_RETAIN)
		return -EINVAL;

	return  __qcom_tee_object_do_invoke(oic, object, op, u, result);
}
EXPORT_SYMBOL_GPL(qcom_tee_object_do_invoke);

/* Dump object table. */
static ssize_t qcom_tee_object_table_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	struct qcom_tee_object *object;
	unsigned long idx;
	size_t len = 0;

	xa_for_each_start(&xa_qcom_local_objects, idx, object, QCOM_TEE_OBJECT_ID_START) {
		len += sysfs_emit_at(buf, len, "%4lx %4d %s\n", idx,
				     kref_read(&object->refcount),
				     qcom_tee_object_name(object));
	}

	return len;
}

static struct kobj_attribute object_table = __ATTR_RO(qcom_tee_object_table);
static struct kobj_attribute release = __ATTR_RO(qcom_tee_release_wq);
static struct attribute *attrs[] = {
	&object_table.attr,
	&release.attr,
	NULL
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *qcom_tee_object_invoke_kobj;
static int __init qcom_tee_object_invoke_init(void)
{
	int ret;

	ret = qcom_tee_release_init();
	if (ret)
		return ret;

	ret = qcom_tee_msg_buffers_init();
	if (ret)
		goto err_release_destroy;

	/* Create '/sys/firmware/qcom_tee'. */
	qcom_tee_object_invoke_kobj = kobject_create_and_add("qcom_tee", firmware_kobj);
	if (!qcom_tee_object_invoke_kobj) {
		ret = -ENOMEM;

		goto err_msg_buffers_destroy;
	}

	/* Create 'qcom_tee_object_table' and 'qcom_tee_release_wq'. */
	ret = sysfs_create_group(qcom_tee_object_invoke_kobj, &attr_group);
	if (ret)
		goto err_kobject_put;

	return 0;

err_kobject_put:
	/* Remove '/sys/firmware/qcom_tee'. */
	kobject_put(qcom_tee_object_invoke_kobj);
err_msg_buffers_destroy:
	qcom_tee_msg_buffers_destroy();
err_release_destroy:
	qcom_tee_release_destroy();

	return ret;
}
module_init(qcom_tee_object_invoke_init);

static void __exit qcom_tee_object_invoke_deinit(void)
{
	/* Wait for RELEASE operations for QTEE objects. */
	qcom_tee_release_destroy();
	qcom_tee_msg_buffers_destroy();
	sysfs_remove_group(qcom_tee_object_invoke_kobj, &attr_group);
	kobject_put(qcom_tee_object_invoke_kobj);
}
module_exit(qcom_tee_object_invoke_deinit);

MODULE_AUTHOR("Qualcomm");
MODULE_DESCRIPTION("QTEE driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
