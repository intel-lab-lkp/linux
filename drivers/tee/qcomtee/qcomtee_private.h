/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef QCOM_TEE_PRIVATE_H
#define QCOM_TEE_PRIVATE_H

#include <linux/firmware/qcom/qcom_tee.h>
#include <linux/kobject.h>
#include <linux/tee_core.h>

/* Flags relating to object reference. */
#define QCOM_TEE_OBJREF_FLAG_USER 1

/* Reserved OBJREF operations. */
/* These operations are not sent to QTEE and handled in driver. */
#define QCOM_TEE_OBJREF_OP_MIN USHRT_MAX
#define QCOM_TEE_OBJREF_OP_RELEASE (QCOM_TEE_OBJREF_OP_MIN + 1)

struct qcom_tee_object *qcom_tee_idx_erase(u32 idx);
void qcom_tee_object_free(struct qcom_tee_object *object);

/* Process async messages form QTEE. */
void qcom_tee_fetch_async_reqs(struct qcom_tee_object_invoke_ctx *oic);

int qcom_tee_release_init(void);
void qcom_tee_release_destroy(void);
void qcom_tee_release_tee_object(struct qcom_tee_object *object);
ssize_t qcom_tee_release_wq_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf);

/* SCM call. */
int qcom_tee_object_invoke_ctx_invoke(struct qcom_tee_object_invoke_ctx *oic,
				      int *result, u64 *response_type);

/**
 * __qcom_tee_object_do_invoke() - Submit an invocation for an object.
 * @oic: context to use for current invocation.
 * @object: object being invoked.
 * @op: requested operation on object.
 * @u: array of argument for the current invocation.
 * @result: result returned from QTEE.
 *
 * Same as qcom_tee_object_do_invoke() without @object and @op is 32-bit,
 * upper 16-bits are for internal use.
 *
 * Return: On success return 0. On error returns -EINVAL and -ENOSPC if unable to initiate
 * the invocation, -EAGAIN if invocation failed and user can retry the invocation.
 * Otherwise, -ENODEV on fatal failure.
 */
int __qcom_tee_object_do_invoke(struct qcom_tee_object_invoke_ctx *oic,
				struct qcom_tee_object *object, u32 op,	struct qcom_tee_arg *u,
				int *result);

/**
 * struct qcom_tee_context - Clients or supplicants context.
 * @tee_context: TEE context.
 * @qtee_objects_idr: QTEE objects in this context.
 * @reqs_idr: Requests currently being processed.
 * @lock: mutex for @reqs_idr and @qtee_objects_idr.
 * @req_srcu: srcu for exclusive access to requests.
 * @req_c: completion used when supplicant is waiting for requests.
 * @released: state of this context.
 * @ref_cnt: ref count.
 */
struct qcom_tee_context {
	struct tee_context *tee_context;

	struct idr qtee_objects_idr;
	struct idr reqs_idr;
	/* Synchronize access to @reqs_idr, @qtee_objects_idr and updating requests state. */
	struct mutex lock;
	struct srcu_struct req_srcu;
	struct completion req_c;

	int released;

	struct kref ref_cnt;
};

void __qcom_tee_context_destroy(struct kref *ref_cnt);

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
				     struct qcom_tee_context *ctx);

/* Get the QTEE object added with qcom_tee_context_add_qtee_object(). */
int qcom_tee_context_find_qtee_object(struct qcom_tee_object **object, struct tee_param *param,
				      struct qcom_tee_context *ctx);

/**
 * qcom_tee_context_del_qtee_object() - Delete a QTEE object from the context.
 * @param: TEE parameter represents @object.
 * @ctx: context to delete the object.
 *
 * @param returned by qcom_tee_context_add_qtee_object().
 */
void qcom_tee_context_del_qtee_object(struct tee_param *param, struct qcom_tee_context *ctx);

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
			   struct qcom_tee_context *ctx);

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
			     struct qcom_tee_context *ctx);

int qcom_tee_driver_register(void);
void qcom_tee_driver_unregister(void);

/* OBJECTS: */

/* (1) Primordial Object. */
extern struct qcom_tee_object qcom_tee_primordial_object;

/* (2) User Object API. */

/* Is it a user object? */
int is_qcom_tee_user_object(struct qcom_tee_object *object);

/* Set user object's 'notify on release' flag. */
void qcom_tee_user_object_set_notify(struct qcom_tee_object *object, bool notify);

/**
 * qcom_tee_user_param_to_object() - Convert OBJREF parameter to &struct qcom_tee_object.
 * @object: object returned.
 * @param: TEE parameter.
 * @ctx: context in which the conversion should happen.
 *
 * @param is OBJREF with %TEE_IOCTL_OBJREF_USER flags.
 *
 * Return: On success return 0 or <0 on failure.
 */
int qcom_tee_user_param_to_object(struct qcom_tee_object **object, struct tee_param *param,
				  struct qcom_tee_context *ctx);

/* Reverse what qcom_tee_user_param_to_object() does. */
int qcom_tee_user_param_from_object(struct tee_param *param, struct qcom_tee_object *object,
				    struct qcom_tee_context *ctx);

struct qcom_tee_user_object_request_data {
	int id;				/* Id assigned to the request. */
	u64 object_id;			/* Object id being invoked by QTEE. */
	u32 op;				/* Requested operation on object. */
	int np;				/* Number of parameters in the request.*/
};

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
			     struct qcom_tee_user_object_request_data *data);

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
				struct tee_param *params, int num_params, int id, int errno);

/**
 * qcom_tee_requests_destroy() - Destroy requests in a context.
 * @ctx: context for which to destroy requests.
 *
 * After calling qcom_tee_requests_destroy(), @ctx can not be reused.
 * It should be called on @ctx cleanup path.
 */
void qcom_tee_requests_destroy(struct qcom_tee_context *ctx);

#endif /* QCOM_TEE_PRIVATE_H */
