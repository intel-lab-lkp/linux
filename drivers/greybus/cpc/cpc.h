/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_H
#define __CPC_H

#include <linux/device.h>
#include <linux/greybus.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct sk_buff;

/**
 * struct cpc_cport - CPC cport
 * @id: cport ID
 * @cpc_hd: pointer to the CPC host device this cport belongs to
 * @lock: mutex to synchronize accesses to tcb and other attributes
 * @tcb: Transmission Control Block
 */
struct cpc_cport {
	u16 id;

	struct cpc_host_device *cpc_hd;
	struct mutex lock; /* Synchronize access to state variables */

	/*
	 * @ack: current acknowledge number
	 * @seq: current sequence number
	 */
	struct {
		u8 ack;
		u8 seq;
	} tcb;
};

struct cpc_cport *cpc_cport_alloc(u16 cport_id, gfp_t gfp_mask);
void cpc_cport_release(struct cpc_cport *cport);

void cpc_cport_pack(struct gb_operation_msg_hdr *gb_hdr, u16 cport_id);
u16 cpc_cport_unpack(struct gb_operation_msg_hdr *gb_hdr);

int cpc_cport_transmit(struct cpc_cport *cport, struct sk_buff *skb);

struct cpc_skb_cb {
	struct cpc_cport *cport;

	/* Keep track of the GB message the skb originates from */
	struct gb_message *gb_message;

	u8 seq;
};

#define CPC_SKB_CB(__skb) ((struct cpc_skb_cb *)&((__skb)->cb[0]))

void cpc_protocol_prepare_header(struct sk_buff *skb, u8 ack);
void cpc_protocol_on_data(struct cpc_cport *cport, struct sk_buff *skb);

#endif
