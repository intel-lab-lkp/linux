/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_H
#define __CPC_H

#include <linux/device.h>
#include <linux/greybus.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/types.h>

/**
 * struct cpc_cport - CPC cport
 * @id: cport ID
 * @cpc_hd: pointer to the CPC host device this cport belongs to
 * @lock: mutex to synchronize accesses to tcb and other attributes
 * @holding_queue: list of frames queued to be sent
 * @retx_queue: list of frames sent and waiting for acknowledgment
 * @tcb: Transmission Control Block
 */
struct cpc_cport {
	u16 id;

	struct cpc_host_device *cpc_hd;
	struct mutex lock; /* Synchronize access to state variables */

	struct sk_buff_head holding_queue;
	struct sk_buff_head retx_queue;

	/*
	 * @send_wnd: send window, maximum number of frames that the remote can accept.
	 *            TX frames should have a sequence in the range [send_una; send_una + send_wnd]
	 * @send_nxt: send next, the next sequence number that will be used for transmission
	 * @send_una: send unacknowledged, the oldest unacknowledged sequence number
	 * @ack: current acknowledge number
	 * @seq: current sequence number
	 */
	struct {
		u8 send_wnd;
		u8 send_nxt;
		u8 send_una;
		u8 ack;
		u8 seq;
	} tcb;
};

struct cpc_cport *cpc_cport_alloc(u16 cport_id, gfp_t gfp_mask);
void cpc_cport_release(struct cpc_cport *cport);

void cpc_cport_pack(struct gb_operation_msg_hdr *gb_hdr, u16 cport_id);
u16 cpc_cport_unpack(struct gb_operation_msg_hdr *gb_hdr);

void cpc_cport_transmit(struct cpc_cport *cport, struct sk_buff *skb);

struct cpc_skb_cb {
	struct cpc_cport *cport;

	/* Keep track of the GB message the skb originates from */
	struct gb_message *gb_message;

	u8 seq;

#define CPC_SKB_FLAG_REQ_ACK (1 << 0)
	u8 cpc_flags;
};

#define CPC_SKB_CB(__skb) ((struct cpc_skb_cb *)&((__skb)->cb[0]))

void cpc_protocol_on_data(struct cpc_cport *cport, struct sk_buff *skb);
void __cpc_protocol_write_head(struct cpc_cport *cport);

#endif
