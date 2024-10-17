/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Linaro Ltd.
 */

#ifndef __MAILBOX_REQUEST_H
#define __MAILBOX_REQUEST_H

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/completion.h>

struct mbox_wait {
	struct completion completion;
	int err;
};

#define DECLARE_MBOX_WAIT(_wait) \
	struct mbox_wait _wait = { \
		COMPLETION_INITIALIZER_ONSTACK((_wait).completion), 0 }

#define MBOX_REQ_MAY_SLEEP	BIT(0)

struct mbox_request {
	struct mbox_wait *wait;
	void *tx;
	void *rx;
	unsigned int txlen;
	unsigned int rxlen;
	u32 flags;
};

#endif /* __MAILBOX_H */
