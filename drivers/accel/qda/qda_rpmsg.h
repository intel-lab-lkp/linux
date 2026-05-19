/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_RPMSG_H__
#define __QDA_RPMSG_H__

#include "qda_drv.h"
#include "qda_fastrpc.h"

/**
 * struct qda_invoke_rsp - Response structure for FastRPC invocations
 */
struct qda_invoke_rsp {
	/** @ctx: Invoke caller context for matching request/response */
	u64 ctx;
	/** @retval: Return value from the remote invocation */
	int retval;
};

/* RPMsg transport layer functions */
int qda_rpmsg_send_msg(struct qda_dev *qdev, struct qda_msg *msg);
int qda_rpmsg_wait_for_rsp(struct fastrpc_invoke_context *ctx);

/* RPMsg transport layer registration */
int qda_rpmsg_register(void);
void qda_rpmsg_unregister(void);

#endif /* __QDA_RPMSG_H__ */
