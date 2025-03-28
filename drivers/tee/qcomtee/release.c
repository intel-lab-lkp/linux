// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "qcomtee_private.h"

static void qcomtee_destroy_user_object(struct work_struct *work)
{
	struct qcomtee_object *object;
	struct qcomtee *qcomtee;
	int ret, result;

	static struct qcomtee_object_invoke_ctx oic;
	/* RELEASE does not require any argument. */
	static struct qcomtee_arg args[] = { { .type = QCOMTEE_ARG_TYPE_INV } };

	object = container_of(work, struct qcomtee_object, work);
	qcomtee = tee_get_drvdata(object->info.qcomtee_async_ctx->teedev);
	/* Get the TEE context used for asynchronous operations. */
	oic.ctx = object->info.qcomtee_async_ctx;

	ret = qcomtee_object_do_invoke_internal(&oic, object,
						QCOMTEE_MSG_OBJECT_OP_RELEASE,
						args, &result);

	/* Is it safe to retry the release? */
	if (ret == -EAGAIN) {
		queue_work(qcomtee->wq, &object->work);
	} else {
		if (ret || result)
			pr_err("%s: %s release failed, ret = %d (%x).\n",
			       __func__, qcomtee_object_name(object), ret,
			       result);

		qcomtee_qtee_object_free(object);
	}
}

/* qcomtee_release_tee_object puts object in release work queue. */
void qcomtee_release_tee_object(struct qcomtee_object *object)
{
	struct qcomtee *qcomtee =
		tee_get_drvdata(object->info.qcomtee_async_ctx->teedev);

	INIT_WORK(&object->work, qcomtee_destroy_user_object);
	queue_work(qcomtee->wq, &object->work);
}
