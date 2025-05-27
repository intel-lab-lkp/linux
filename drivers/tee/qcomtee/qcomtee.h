/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef QCOMTEE_H
#define QCOMTEE_H

#include <linux/kobject.h>
#include <linux/tee_core.h>

#include "qcomtee_msg.h"
#include "qcomtee_object.h"

/* Flags relating to object reference. */
#define QCOMTEE_OBJREF_FLAG_TEE		BIT(0)
#define QCOMTEE_OBJREF_FLAG_USER	BIT(1)
#define QCOMTEE_OBJREF_FLAG_MEM		BIT(2)

/**
 * struct qcomtee - Main service struct.
 * @teedev: client device.
 * @pool: shared memory pool.
 * @ctx: driver private context.
 * @oic: context to use for the current driver invocation.
 * @wq: workqueue for QTEE async operations.
 * @xa_local_objects: array of objects exported to QTEE.
 * @xa_last_id: next ID to allocate.
 */
struct qcomtee {
	struct tee_device *teedev;
	struct tee_shm_pool *pool;
	struct tee_context *ctx;
	struct qcomtee_object_invoke_ctx oic;
	struct workqueue_struct *wq;
	struct xarray xa_local_objects;
	u32 xa_last_id;
};

void qcomtee_fetch_async_reqs(struct qcomtee_object_invoke_ctx *oic);
struct qcomtee_object *qcomtee_idx_erase(struct qcomtee_object_invoke_ctx *oic,
					 u32 idx);

struct tee_shm_pool *qcomtee_shm_pool_alloc(void);
void qcomtee_msg_buffers_free(struct qcomtee_object_invoke_ctx *oic);
int qcomtee_msg_buffers_alloc(struct qcomtee_object_invoke_ctx *oic,
			      struct qcomtee_arg *u);

/**
 * qcomtee_object_do_invoke_internal() - Submit an invocation for an object.
 * @oic: context to use for the current invocation.
 * @object: object being invoked.
 * @op: requested operation on the object.
 * @u: array of arguments for the current invocation.
 * @result: result returned from QTEE.
 *
 * The caller is responsible for keeping track of the refcount for each
 * object, including @object. On return, the caller loses ownership of all
 * input objects of type %QCOMTEE_OBJECT_TYPE_CB.
 *
 * Return: On success, returns 0. On error, returns -EAGAIN if invocation
 * failed and the user may retry the invocation, -ENODEV on fatal failure.
 */
int qcomtee_object_do_invoke_internal(struct qcomtee_object_invoke_ctx *oic,
				      struct qcomtee_object *object, u32 op,
				      struct qcomtee_arg *u, int *result);

/**
 * struct qcomtee_context_data - Clients' or supplicants' context.
 * @qtee_objects_idr: QTEE objects in this context.
 * @qtee_lock: mutex for @qtee_objects_idr.
 * @reqs_idr: requests in this context that hold ID.
 * @reqs_list: FIFO for requests in PROCESSING or QUEUED state.
 * @reqs_lock: mutex for @reqs_idr, @reqs_list and request states.
 * @req_c: completion used when the supplicant is waiting for requests.
 * @released: state of this context.
 */
struct qcomtee_context_data {
	struct idr qtee_objects_idr;
	/* Synchronize access to @qtee_objects_idr. */
	struct mutex qtee_lock;

	struct idr reqs_idr;
	struct list_head reqs_list;
	/* Synchronize access to @reqs_idr, @reqs_list and updating requests states. */
	struct mutex reqs_lock;

	struct completion req_c;

	bool released;
};

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
				    struct tee_context *ctx);

/* Retrieve the QTEE object added with qcomtee_context_add_qtee_object(). */
int qcomtee_context_find_qtee_object(struct qcomtee_object **object,
				     struct tee_param *param,
				     struct tee_context *ctx);

/**
 * qcomtee_context_del_qtee_object() - Delete a QTEE object from the context.
 * @param: TEE parameter representing @object.
 * @ctx: context for deleting the object.
 *
 * The @param has been initialized by qcomtee_context_add_qtee_object().
 */
void qcomtee_context_del_qtee_object(struct tee_param *param,
				     struct tee_context *ctx);

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
			  struct tee_context *ctx);

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
			    struct tee_context *ctx);

/* OBJECTS: */

/* (1) User Object API. */

/* Is it a user object? */
int is_qcomtee_user_object(struct qcomtee_object *object);

/* Set the user object's 'notify on release' flag. */
void qcomtee_user_object_set_notify(struct qcomtee_object *object, bool notify);

/* This is called when there are no more users for the ctxdata. */
void qcomtee_requests_destroy(struct qcomtee_context_data *ctxdata);

/**
 * qcomtee_user_param_to_object() - OBJREF parameter to &struct qcomtee_object.
 * @object: object returned.
 * @param: TEE parameter.
 * @ctx: context in which the conversion should happen.
 *
 * @param is an OBJREF with %QCOMTEE_OBJREF_FLAG_USER flags.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
int qcomtee_user_param_to_object(struct qcomtee_object **object,
				 struct tee_param *param,
				 struct tee_context *ctx);

/* Reverse what qcomtee_user_param_to_object() does. */
int qcomtee_user_param_from_object(struct tee_param *param,
				   struct qcomtee_object *object,
				   struct tee_context *ctx);

struct qcomtee_user_object_request_data {
	int id; /* ID assigned to the request. */
	u64 object_id; /* Object ID being invoked by QTEE. */
	u32 op; /* Requested operation on object. */
	int np; /* Number of parameters in the request.*/
};

/**
 * qcomtee_user_object_select() - Select a request for a user object.
 * @ctx: context to look for a user object.
 * @params: parameters for @op.
 * @num_params: number of elements in the parameter array.
 * @uaddr: user buffer for output UBUF parameters.
 * @size: size of user buffer @uaddr.
 * @data: information for the selected request.
 *
 * @params is filled along with @data for the selected request.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
int qcomtee_user_object_select(struct tee_context *ctx,
			       struct tee_param *params, int num_params,
			       void __user *uaddr, size_t size,
			       struct qcomtee_user_object_request_data *data);

/**
 * qcomtee_user_object_submit() - Submit a response for a user object.
 * @ctx: context to look for a user object.
 * @params: returned parameters.
 * @num_params: number of elements in the parameter array.
 * @req_id: request ID for the response.
 * @errno: result of user object invocation.
 *
 * Return: On success, returns 0; on failure, returns < 0.
 */
int qcomtee_user_object_submit(struct tee_context *ctx,
			       struct tee_param *params, int num_params,
			       int req_id, int errno);

/* (2) Primordial Object. */
extern struct qcomtee_object qcomtee_primordial_object;

/* (3) Memory Object API. */

/* Is it a memory object using tee_shm? */
int is_qcomtee_memobj_object(struct qcomtee_object *object);

/**
 * qcomtee_memobj_param_to_object() - OBJREF parameter to &struct qcomtee_object.
 * @object: object returned.
 * @param: TEE parameter.
 * @ctx: context in which the conversion should happen.
 *
 * @param is an OBJREF with %QCOMTEE_OBJREF_FLAG_MEM flags.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcomtee_memobj_param_to_object(struct qcomtee_object **object,
				   struct tee_param *param,
				   struct tee_context *ctx);

/* Reverse what qcomtee_memobj_param_to_object() does. */
int qcomtee_memobj_param_from_object(struct tee_param *param,
				     struct qcomtee_object *object,
				     struct tee_context *ctx);

/**
 * qcomtee_mem_object_map() - Map a memory object.
 * @object: memory object.
 * @map_object: created mapping object.
 * @mem_paddr: physical address of the memory.
 * @mem_size: size of the memory.
 * @perms: QTEE access permissions.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcomtee_mem_object_map(struct qcomtee_object *object,
			   struct qcomtee_object **map_object, u64 *mem_paddr,
			   u64 *mem_size, u32 *perms);

#endif /* QCOMTEE_H */
