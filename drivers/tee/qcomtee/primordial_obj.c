// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include "qcomtee_private.h"

/**
 * DOC: Primordial Object
 *
 * After the boot, REE provides a static object of type %QCOM_TEE_OBJECT_TYPE_CB_OBJECT
 * called primordial object. This object is used for native REE services or privileged operations.
 *
 * We support
 *  - %QCOM_TEE_OBJECT_OP_YIELD to yield by the thread running in QTEE.
 *  - %QCOM_TEE_OBJECT_OP_SLEEP to wait for period of time.
 */

#define QCOM_TEE_OBJECT_OP_YIELD	1
#define QCOM_TEE_OBJECT_OP_SLEEP	2

static int qcom_tee_primordial_object_dispatch(struct qcom_tee_object_invoke_ctx *oic,
					       struct qcom_tee_object *primordial_object_unused,
					       u32 op, struct qcom_tee_arg *args)
{
	int err = 0;

	switch (op) {
	case QCOM_TEE_OBJECT_OP_YIELD:
		cond_resched();
		/* No output object. */
		oic->data = NULL;
		break;
	case QCOM_TEE_OBJECT_OP_SLEEP:
		/* Check message format matched QCOM_TEE_OBJECT_OP_SLEEP op. */
		if (qcom_tee_args_len(args) != 1 ||		/* Expect 1 argument. */
		    args[0].type != QCOM_TEE_ARG_TYPE_IB ||	/* Time to sleep in ms. */
		    args[0].b.size < sizeof(u32))		/* Buffer should hold a u32. */
			return -EINVAL;

		msleep(*(u32 *)(args[0].b.addr));
		/* No output object. */
		oic->data = NULL;
		break;
	default:
		err = -EINVAL;
	}

	return err;
}

static struct qcom_tee_object_operations qcom_tee_primordial_object_ops = {
	.dispatch = qcom_tee_primordial_object_dispatch,
};

struct qcom_tee_object qcom_tee_primordial_object = {
	.name = "primordial",
	.object_type = QCOM_TEE_OBJECT_TYPE_CB_OBJECT,
	.ops = &qcom_tee_primordial_object_ops
};
