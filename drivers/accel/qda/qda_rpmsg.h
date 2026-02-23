/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_RPMSG_H__
#define __QDA_RPMSG_H__

#include "qda_drv.h"
#include "qda_fastrpc.h"

/**
 * struct fastrpc_msg - FastRPC message structure for remote invocations
 *
 * This structure represents a FastRPC message sent to the remote processor
 * via RPMsg transport layer.
 */
struct fastrpc_msg {
	/* Process client ID */
	int client_id;
	/* Thread ID */
	int tid;
	/* Context identifier for matching request/response */
	u64 ctx;
	/* Handle to invoke on remote processor */
	u32 handle;
	/* Scalars structure describing the data layout */
	u32 sc;
	/* Physical address of the message buffer */
	u64 addr;
	/* Size of contiguous region */
	u64 size;
};

/**
 * struct qda_invoke_rsp - Response structure for FastRPC invocations
 */
struct qda_invoke_rsp {
	/* Invoke caller context for matching request/response */
	u64 ctx;
	/* Return value from the remote invocation */
	int retval;
};

/*
 * RPMsg transport layer functions
 */
int qda_rpmsg_send_msg(struct qda_dev *qdev, struct qda_msg *msg);
int qda_rpmsg_wait_for_rsp(struct fastrpc_invoke_context *ctx);

/*
 * Transport layer registration
 */
int qda_rpmsg_register(void);
void qda_rpmsg_unregister(void);

#endif /* __QDA_RPMSG_H__ */
