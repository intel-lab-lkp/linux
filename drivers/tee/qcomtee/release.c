// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "qcomtee_private.h"
#include "qcomtee_msg.h"

static struct workqueue_struct *qcom_tee_release_wq;

/* Number of all release requests pending for processing. */
static atomic_t qcom_tee_pending_releases = ATOMIC_INIT(0);

/* qcom_tee_object_do_release makes direct object invocation to release an object. */
static void qcom_tee_destroy_user_object(struct work_struct *work)
{
	static struct qcom_tee_object_invoke_ctx oic;
	static struct qcom_tee_arg args[1] = { 0 };
	struct qcom_tee_object *object;
	int ret, result;

	object = container_of(work, struct qcom_tee_object, work);

	ret = __qcom_tee_object_do_invoke(&oic, object, QCOM_TEE_MSG_OBJECT_OP_RELEASE, args,
					  &result);

	/* Is it safe to retry the release? */
	if (ret == -EAGAIN) {
		queue_work(qcom_tee_release_wq, &object->work);
	} else {
		if (ret || result)
			pr_err("%s: %s release failed, ret = %d (%x).\n",
			       __func__, qcom_tee_object_name(object), ret, result);

		atomic_dec(&qcom_tee_pending_releases);
		qcom_tee_object_free(object);
	}
}

/* qcom_tee_release_tee_object puts object in release work queue. */
void qcom_tee_release_tee_object(struct qcom_tee_object *object)
{
	INIT_WORK(&object->work, qcom_tee_destroy_user_object);
	atomic_inc(&qcom_tee_pending_releases);
	queue_work(qcom_tee_release_wq, &object->work);
}

ssize_t qcom_tee_release_wq_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&qcom_tee_pending_releases));
}

int qcom_tee_release_init(void)
{
	qcom_tee_release_wq = alloc_ordered_workqueue("qcom_tee_release_wq", 0);
	if (!qcom_tee_release_wq)
		return -ENOMEM;

	return 0;
}

void qcom_tee_release_destroy(void)
{
	/* It drains the wq. */
	destroy_workqueue(qcom_tee_release_wq);
}
