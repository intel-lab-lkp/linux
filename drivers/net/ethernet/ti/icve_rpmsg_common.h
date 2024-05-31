/* SPDX-License-Identifier: GPL-2.0
 * Texas Instruments K3 Inter Core Virtual Ethernet Driver common header
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef __ICVE_RPMSG_COMMON_H__
#define __ICVE_RPMSG_COMMON_H__

#include <linux/if_ether.h>

enum icve_msg_type {
	ICVE_REQUEST_MSG = 0,
	ICVE_RESPONSE_MSG,
	ICVE_NOTIFY_MSG,
};

struct request_message {
	u32 type; /* Request Type */
	u32 id;	  /* Request ID */
} __packed;

struct response_message {
	u32 type;	/* Response Type */
	u32 id;		/* Response ID */
} __packed;

struct notify_message {
	u32 type;	/* Notify Type */
	u32 id;		/* Notify ID */
} __packed;

struct message_header {
	u32 src_id;
	u32 msg_type; /* Do not use enum type, as enum size is compiler dependent */
} __packed;

struct message {
	struct message_header msg_hdr;
	union {
		struct request_message req_msg;
		struct response_message resp_msg;
		struct notify_message notify_msg;
	};
} __packed;

#endif /* __ICVE_RPMSG_COMMON_H__ */
