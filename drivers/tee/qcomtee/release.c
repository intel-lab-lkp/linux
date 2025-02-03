// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "qcomtee_private.h"
#include "qcomtee_msg.h"

static struct workqueue_struct *qcomtee_release_wq;

static void qcomtee_destroy_user_object(struct work_struct *work)
{
	static struct qcomtee_object_invoke_ctx oic;
	struct qcomtee_object *object;
	int ret, result;

	/* RELEASE does not require any argument. */
	static struct qcomtee_arg args[] = { { .type = QCOMTEE_ARG_TYPE_INV } };

	object = container_of(work, struct qcomtee_object, work);

	ret = qcomtee_object_do_invoke_internal(&oic, object,
						QCOMTEE_MSG_OBJECT_OP_RELEASE,
						args, &result);

	/* Is it safe to retry the release? */
	if (ret == -EAGAIN) {
		queue_work(qcomtee_release_wq, &object->work);
	} else {
		if (ret || result)
			pr_err("%s: %s release failed, ret = %d (%x).\n",
			       __func__, qcomtee_object_name(object), ret,
			       result);

		qcomtee_object_free(object);
	}
}

/* qcomtee_release_tee_object puts object in release work queue. */
void qcomtee_release_tee_object(struct qcomtee_object *object)
{
	INIT_WORK(&object->work, qcomtee_destroy_user_object);
	queue_work(qcomtee_release_wq, &object->work);
}

int qcomtee_release_init(void)
{
	qcomtee_release_wq = alloc_ordered_workqueue("qcomtee_release_wq", 0);
	if (!qcomtee_release_wq)
		return -ENOMEM;

	return 0;
}

void qcomtee_release_destroy(void)
{
	/* It drains the wq. */
	destroy_workqueue(qcomtee_release_wq);
}
