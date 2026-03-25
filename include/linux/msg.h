/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MSG_H
#define _LINUX_MSG_H

#include <linux/list.h>
#include <uapi/linux/msg.h>

/*
 * Each message is stored in one or more page-sized segments.
 * The first segment is embedded in struct msg_msg; overflow goes into
 * chained struct msg_msgseg blocks.
 */
struct msg_msgseg {
	struct msg_msgseg *next;
	/* message data follows immediately */
};

#define DATALEN_MSG	((size_t)PAGE_SIZE - sizeof(struct msg_msg))
#define DATALEN_SEG	((size_t)PAGE_SIZE - sizeof(struct msg_msgseg))

/* one msg_msg structure for each message */
struct msg_msg {
	struct list_head m_list;
	long m_type;
	size_t m_ts;		/* message text size */
	struct msg_msgseg *next;
	void *security;
	/* the actual message follows immediately */
};

#endif /* _LINUX_MSG_H */
