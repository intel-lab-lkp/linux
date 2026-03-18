/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __XE_SYSCTRL_MAILBOX_H__
#define __XE_SYSCTRL_MAILBOX_H__

#include <linux/bitfield.h>
#include <linux/types.h>

struct xe_sysctrl;
struct xe_device;
struct xe_sysctrl_mailbox_command;

#define APP_HDR_GROUP_ID_MASK			GENMASK(7, 0)
#define APP_HDR_COMMAND_MASK			GENMASK(15, 8)
#define APP_HDR_VERSION_MASK			GENMASK(23, 16)
#define APP_HDR_RESERVED_MASK			GENMASK(31, 24)

#define XE_SYSCTRL_APP_HDR_GROUP_ID(hdr) \
	FIELD_GET(APP_HDR_GROUP_ID_MASK, le32_to_cpu((hdr)->data))

#define XE_SYSCTRL_APP_HDR_COMMAND(hdr) \
	FIELD_GET(APP_HDR_COMMAND_MASK, le32_to_cpu((hdr)->data))

#define XE_SYSCTRL_APP_HDR_VERSION(hdr) \
	FIELD_GET(APP_HDR_VERSION_MASK, le32_to_cpu((hdr)->data))

void xe_sysctrl_mailbox_init(struct xe_sysctrl *sc);
int xe_sysctrl_send_command(struct xe_device *xe,
			    struct xe_sysctrl_mailbox_command *cmd,
			    size_t *rdata_len);

#endif /* __XE_SYSCTRL_MAILBOX_H__ */
