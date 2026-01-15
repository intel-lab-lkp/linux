// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/skbuff.h>

#include "cpc.h"
#include "header.h"
#include "host.h"

static bool cpc_skb_is_sequenced(struct sk_buff *skb)
{
	return CPC_SKB_CB(skb)->cpc_flags & CPC_SKB_FLAG_REQ_ACK;
}

static void cpc_protocol_prepare_header(struct sk_buff *skb, u8 ack)
{
	struct cpc_header *hdr;

	skb_push(skb, sizeof(*hdr));

	hdr = (struct cpc_header *)skb->data;
	hdr->ack = ack;
	hdr->recv_wnd = 0;
	hdr->seq = CPC_SKB_CB(skb)->seq;
	hdr->ctrl_flags = cpc_header_encode_ctrl_flags(!CPC_SKB_CB(skb)->gb_message,
						       cpc_skb_is_sequenced(skb));
}

static void cpc_protocol_queue_ack(struct cpc_cport *cport, u8 ack)
{
	struct gb_operation_msg_hdr *gb_hdr;
	struct sk_buff *skb;

	skb = alloc_skb(CPC_HEADER_SIZE + sizeof(*gb_hdr), GFP_KERNEL);
	if (!skb)
		return;

	skb_reserve(skb, CPC_HEADER_SIZE);

	gb_hdr = skb_put(skb, sizeof(*gb_hdr));
	memset(gb_hdr, 0, sizeof(*gb_hdr));

	/* In the CPC Operation Header, only the size and cport_id matter for ACKs. */
	gb_hdr->size = cpu_to_le16(sizeof(*gb_hdr));
	cpc_cport_pack(gb_hdr, cport->id);

	cpc_protocol_prepare_header(skb, ack);

	cpc_hd_send_skb(cport->cpc_hd, skb);
}

void cpc_protocol_on_data(struct cpc_cport *cport, struct sk_buff *skb)
{
	struct cpc_header *cpc_hdr = (struct cpc_header *)skb->data;
	bool require_ack = cpc_header_get_req_ack(cpc_hdr);
	u8 seq = cpc_header_get_seq(cpc_hdr);
	bool expected_seq = false;
	u8 ack;

	mutex_lock(&cport->lock);

	if (require_ack) {
		expected_seq = seq == cport->tcb.ack;
		if (expected_seq)
			cport->tcb.ack++;
		else
			dev_warn_ratelimited(cpc_hd_dev(cport->cpc_hd),
					     "unexpected seq: %u, expected seq: %u\n",
					     seq, cport->tcb.ack);
	}

	ack = cport->tcb.ack;

	mutex_unlock(&cport->lock);

	/* Ack no matter if the sequence was valid or not, to resync with remote */
	if (require_ack)
		cpc_protocol_queue_ack(cport, ack);

	if (expected_seq && !cpc_header_is_control(cpc_hdr)) {
		skb_pull(skb, CPC_HEADER_SIZE);

		greybus_data_rcvd(cport->cpc_hd->gb_hd, cport->id, skb->data, skb->len);
	}
}

static void __cpc_protocol_write_skb(struct cpc_cport *cport, struct sk_buff *skb, u8 ack)
{
	cpc_protocol_prepare_header(skb, ack);

	cpc_hd_send_skb(cport->cpc_hd, skb);
}

/* Write skbs at the head of holding queue */
void __cpc_protocol_write_head(struct cpc_cport *cport)
{
	struct sk_buff *skb;
	u8 ack;

	ack = cport->tcb.ack;

	/* For each SKB in the holding queue, clone it and pass it to lower layer */
	while ((skb = skb_peek(&cport->holding_queue))) {
		skb_unlink(skb, &cport->holding_queue);

		__cpc_protocol_write_skb(cport, skb, ack);
	}
}
