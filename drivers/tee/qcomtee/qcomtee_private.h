/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef QCOM_TEE_PRIVATE_H
#define QCOM_TEE_PRIVATE_H

#include <linux/firmware/qcom/qcom_tee.h>
#include <linux/kobject.h>
#include <linux/tee_core.h>

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

#endif /* QCOM_TEE_PRIVATE_H */
