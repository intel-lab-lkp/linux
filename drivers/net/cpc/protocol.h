/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef CPC_PROTOCOL_H
#define CPC_PROTOCOL_H

#include <linux/skbuff.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct cpc_endpoint;
struct cpc_header;

int __cpc_protocol_write(struct cpc_endpoint *ep, struct cpc_header *hdr, struct sk_buff *skb);

void cpc_protocol_on_data(struct cpc_endpoint *ep, struct sk_buff *skb);

#endif
