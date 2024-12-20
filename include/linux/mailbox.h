/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Linaro Ltd.
 */

#ifndef __LINUX_MAILBOX_H
#define __LINUX_MAILBOX_H

#include <linux/types.h>

#define MBOX_XLATE_MAX_ARGS 16
struct mbox_xlate_args {
	int args_count;
	u32 args[MBOX_XLATE_MAX_ARGS];
};

#endif /* __LINUX_MAILBOX_H */
