/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2025 Cisco Systems, Inc.  All rights reserved. */

#ifndef _ENIC_ADMIN_H_
#define _ENIC_ADMIN_H_

#define ENIC_ADMIN_DESC_COUNT	64
#define ENIC_ADMIN_BUF_SIZE	2048

struct enic;

/* Wrapper for received admin messages queued for deferred processing.
 * NAPI enqueues these; a workqueue handler processes them in process context
 * where sleeping (mutex, GFP_KERNEL) is safe.
 */
struct enic_admin_msg {
	struct list_head list;
	unsigned int len;
	u8 data[] __aligned(8);
};

int enic_admin_channel_open(struct enic *enic);
void enic_admin_channel_close(struct enic *enic);
unsigned int enic_admin_wq_cq_service(struct enic *enic);
unsigned int enic_admin_rq_cq_service(struct enic *enic, unsigned int budget);

#endif /* _ENIC_ADMIN_H_ */
