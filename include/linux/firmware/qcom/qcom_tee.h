/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __QCOM_TEE_H
#define __QCOM_TEE_H

#include <linux/kref.h>
#include <linux/completion.h>
#include <linux/workqueue.h>

struct qcom_tee_object;

/**
 * DOC: Overview
 *
 * qcom_tee_object provides object ref-counting, id allocation for objects hosted in
 * REE, and necessary message marshaling for Qualcomm TEE (QTEE).
 *
 * To invoke an object in QTEE, user calls qcom_tee_object_do_invoke() while passing
 * an instance of &struct qcom_tee_object and the requested operation + arguments.
 *
 * After the boot, QTEE provides a static object %ROOT_QCOM_TEE_OBJECT (type of
 * %QCOM_TEE_OBJECT_TYPE_ROOT). The root object is invoked to pass user's credentials and
 * obtain other instances of &struct qcom_tee_object (type of %QCOM_TEE_OBJECT_TYPE_TEE)
 * that represents services and TAs in QTEE, see &enum qcom_tee_object_type.
 *
 * The object received from QTEE are refcounted. So the owner of these objects can
 * issue qcom_tee_object_get(), to increase the refcount, and pass objects to other
 * clients, or issue qcom_tee_object_put() to decrease the refcount, and releasing
 * the resources in QTEE.
 *
 * REE can host services accessible to QTEE. A driver should embed an instance of
 * &struct qcom_tee_object in the struct it wants to export to QTEE (it is called
 * callback object). It issues qcom_tee_object_user_init() to set the dispatch()
 * operation for the callback object and set its type to %QCOM_TEE_OBJECT_TYPE_CB_OBJECT.
 *
 * core.c holds an object table for callback objects. An object id is assigned
 * to each callback object which is an index to the object table. QTEE uses these ids
 * to reference or invoke callback objects.
 *
 * If QTEE invoke a callback object in REE, the dispatch() operation is called in the
 * context of thread that called qcom_tee_object_do_invoke(), originally.
 */

/**
 * enum qcom_tee_object_typ - Object types.
 * @QCOM_TEE_OBJECT_TYPE_TEE: object hosted on QTEE.
 * @QCOM_TEE_OBJECT_TYPE_CB_OBJECT: object hosted on REE.
 * @QCOM_TEE_OBJECT_TYPE_ROOT: 'primordial' object.
 * @QCOM_TEE_OBJECT_TYPE_NULL: NULL object.
 *
 * Primordial object is used for bootstrapping the IPC connection between a REE
 * and QTEE. It is invoked by REE when it wants to get a 'client env'.
 */
enum qcom_tee_object_type {
	QCOM_TEE_OBJECT_TYPE_TEE,
	QCOM_TEE_OBJECT_TYPE_CB_OBJECT,
	QCOM_TEE_OBJECT_TYPE_ROOT,
	QCOM_TEE_OBJECT_TYPE_NULL,
};

/**
 * enum qcom_tee_arg_type - Type of QTEE argument.
 * @QCOM_TEE_ARG_TYPE_INV: invalid type.
 * @QCOM_TEE_ARG_TYPE_IB: input buffer (IO).
 * @QCOM_TEE_ARG_TYPE_OO: output object (OO).
 * @QCOM_TEE_ARG_TYPE_OB: output buffer (OB).
 * @QCOM_TEE_ARG_TYPE_IO: input object (IO).
 *
 * Use invalid type to specify end of argument array.
 */
enum qcom_tee_arg_type {
	QCOM_TEE_ARG_TYPE_INV = 0,
	QCOM_TEE_ARG_TYPE_OB,
	QCOM_TEE_ARG_TYPE_OO,
	QCOM_TEE_ARG_TYPE_IB,
	QCOM_TEE_ARG_TYPE_IO,
	QCOM_TEE_ARG_TYPE_NR,
};

/**
 * define QCOM_TEE_ARGS_PER_TYPE - Maximum arguments of specific type.
 *
 * QTEE transport protocol limits maximum number of argument of specific type
 * (i.e. IB, OB, IO, and OO).
 */
#define QCOM_TEE_ARGS_PER_TYPE 16

/* Maximum arguments that can fit in a QTEE message, ignoring the type. */
#define QCOM_TEE_ARGS_MAX (QCOM_TEE_ARGS_PER_TYPE * (QCOM_TEE_ARG_TYPE_NR - 1))

struct qcom_tee_buffer {
	union {
		void *addr;
		void __user *uaddr;
	};
	size_t size;
};

/**
 * struct qcom_tee_arg - Argument for QTEE object invocation.
 * @type: type of argument as &enum qcom_tee_arg_type.
 * @flags: extra flags.
 * @b: address and size if type of argument is buffer.
 * @o: object instance if type of argument is object.
 *
 * &qcom_tee_arg.flags only accept %QCOM_TEE_ARG_FLAGS_UADDR for now which states
 * that &qcom_tee_arg.b contains userspace address in uaddr.
 */
struct qcom_tee_arg {
	enum qcom_tee_arg_type type;
/* 'b.uaddr' holds a __user address. */
#define QCOM_TEE_ARG_FLAGS_UADDR 1
	unsigned int flags;
	union {
		struct qcom_tee_buffer b;
		struct qcom_tee_object *o;
	};
};

static inline int qcom_tee_args_len(struct qcom_tee_arg *args)
{
	int i = 0;

	while (args[i].type != QCOM_TEE_ARG_TYPE_INV)
		i++;
	return i;
}

#define QCOM_TEE_OIC_FLAG_BUSY		BIT(1)	/* Context is busy (callback is in progress). */
#define QCOM_TEE_OIC_FLAG_NOTIFY	BIT(2)	/* Context needs to notify the current object. */
#define QCOM_TEE_OIC_FLAG_SHARED	BIT(3)	/* Context has shared state with QTEE. */

struct qcom_tee_object_invoke_ctx {
	unsigned long flags;
	int errno;

	/* Current object invoked in this callback context. */
	struct qcom_tee_object *object;

	/* Arguments passed to dispatch callback (+1 for ending QCOM_TEE_ARG_TYPE_INV). */
	struct qcom_tee_arg u[QCOM_TEE_ARGS_MAX + 1];

	/* Inbound and Outbound buffers shared with QTEE. */
	struct qcom_tee_buffer in_msg;		/* Inbound Buffer.  */
	phys_addr_t in_msg_paddr;		/* Physical address of inbound buffer. */
	struct qcom_tee_buffer out_msg;		/* Outbound Buffer. */
	phys_addr_t out_msg_paddr;		/* Physical address of outbound buffer. */

	/* Extra data attached to this context. */
	void *data;
};

/**
 * qcom_tee_object_do_invoke() - Submit an invocation for an object.
 * @oic: context to use for current invocation.
 * @object: object being invoked.
 * @op: requested operation on object.
 * @u: array of argument for the current invocation.
 * @result: result returned from QTEE.
 *
 * The caller is responsible to keep track of the refcount for each object,
 * including @object. On return, the caller loses the ownership of all input object of
 * type %QCOM_TEE_OBJECT_TYPE_CB_OBJECT.
 *
 * @object can be of %QCOM_TEE_OBJECT_TYPE_ROOT or %QCOM_TEE_OBJECT_TYPE_TEE types.
 *
 * Return: On success return 0. On error returns -EINVAL and -ENOSPC if unable to initiate
 * the invocation, -EAGAIN if invocation failed and user may retry the invocation.
 * Otherwise, -ENODEV on fatal failure.
 */
int qcom_tee_object_do_invoke(struct qcom_tee_object_invoke_ctx *oic,
			      struct qcom_tee_object *object, u32 op, struct qcom_tee_arg *u,
			      int *result);

/**
 * struct qcom_tee_object_operations - Callback object operations.
 * @release: release object if QTEE is not using it.
 * @dispatch: dispatch the operation requested by QTEE.
 * @notify: report status of any pending response submitted by @dispatch.
 *
 * Transport may fail (e.g. object table is full) even after @dispatch successfully submitted
 * the response. @notify is called to do the necessary cleanup.
 */
struct qcom_tee_object_operations {
	void (*release)(struct qcom_tee_object *object);
	int  (*dispatch)(struct qcom_tee_object_invoke_ctx *oic,
			 struct qcom_tee_object *object, u32 op, struct qcom_tee_arg *args);
	void (*notify)(struct qcom_tee_object_invoke_ctx *oic,
		       struct qcom_tee_object *object, int err);
};

/**
 * struct qcom_tee_object - QTEE or REE object.
 * @name: object name.
 * @refcount: reference counter.
 * @object_type: object type as &enum qcom_tee_object_type.
 * @info: extra information for object.
 * @owner: owning module/driver.
 * @ops: callback operations for object of type %QCOM_TEE_OBJECT_TYPE_CB_OBJECT.
 * @work: work for async operation on object.
 *
 * @work is currently only used for release object of %QCOM_TEE_OBJECT_TYPE_TEE type.
 */
struct qcom_tee_object {
	const char *name;
	struct kref refcount;

	enum qcom_tee_object_type object_type;
	union object_info {
		/* QTEE object id if object_type is %QCOM_TEE_OBJECT_TYPE_TEE. */
		unsigned long qtee_id;
	} info;

	struct module *owner;
	struct qcom_tee_object_operations *ops;
	struct work_struct work;
};

/* Static instances of qcom_tee_object objects. */
#define NULL_QCOM_TEE_OBJECT ((struct qcom_tee_object *)(0))
extern struct qcom_tee_object qcom_tee_object_root;
#define ROOT_QCOM_TEE_OBJECT (&qcom_tee_object_root)

static inline enum qcom_tee_object_type typeof_qcom_tee_object(struct qcom_tee_object *object)
{
	if (object == NULL_QCOM_TEE_OBJECT)
		return QCOM_TEE_OBJECT_TYPE_NULL;
	return object->object_type;
}

static inline const char *qcom_tee_object_name(struct qcom_tee_object *object)
{
	if (object == NULL_QCOM_TEE_OBJECT)
		return "null";

	if (!object->name)
		return "no-name";
	return object->name;
}

/**
 * __qcom_tee_object_user_init() - Initialize an object for user.
 * @object: object to initialize.
 * @object_type: type of object as &enum qcom_tee_object_type.
 * @ops: instance of callbacks.
 * @owner: owning module/driver.
 * @fmt: name assigned to the object.
 *
 * Return: On success return 0 or <0 on failure.
 */
int __qcom_tee_object_user_init(struct qcom_tee_object *object, enum qcom_tee_object_type ot,
				struct qcom_tee_object_operations *ops, struct module *owner,
				const char *fmt, ...);
#define qcom_tee_object_user_init(obj, ot, ops, fmt, ...) \
	__qcom_tee_object_user_init((obj), (ot), (ops), THIS_MODULE, (fmt), __VA_ARGS__)

/* Object release is RCU protected. */
int qcom_tee_object_get(struct qcom_tee_object *object);
void qcom_tee_object_put(struct qcom_tee_object *object);

#define qcom_tee_arg_for_each(i, args) \
	for (i = 0; args[i].type != QCOM_TEE_ARG_TYPE_INV; i++)

/* Next argument of type @type after index @i. */
int qcom_tee_next_arg_type(struct qcom_tee_arg *u, int i, enum qcom_tee_arg_type type);

/* Iterate over argument of given type. */
#define qcom_tee_arg_for_each_type(i, args, at)			\
	for (i = 0, i = qcom_tee_next_arg_type(args, i, at);	\
		args[i].type != QCOM_TEE_ARG_TYPE_INV;		\
		i++, i = qcom_tee_next_arg_type(args, i, at))

#define qcom_tee_arg_for_each_input_buffer(i, args)  \
	qcom_tee_arg_for_each_type(i, args, QCOM_TEE_ARG_TYPE_IB)
#define qcom_tee_arg_for_each_output_buffer(i, args) \
	qcom_tee_arg_for_each_type(i, args, QCOM_TEE_ARG_TYPE_OB)
#define qcom_tee_arg_for_each_input_object(i, args)  \
	qcom_tee_arg_for_each_type(i, args, QCOM_TEE_ARG_TYPE_IO)
#define qcom_tee_arg_for_each_output_object(i, args) \
	qcom_tee_arg_for_each_type(i, args, QCOM_TEE_ARG_TYPE_OO)

#endif /* __QCOM_TEE_H */
