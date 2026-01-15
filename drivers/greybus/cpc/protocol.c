// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/skbuff.h>

#include "cpc.h"
#include "header.h"
#include "host.h"

void cpc_protocol_prepare_header(struct sk_buff *skb, u8 ack)
{
	struct cpc_header *hdr;

	skb_push(skb, sizeof(*hdr));

	hdr = (struct cpc_header *)skb->data;
	hdr->ack = ack;
	hdr->recv_wnd = 0;
	hdr->ctrl_flags = 0;
	hdr->seq = CPC_SKB_CB(skb)->seq;
}

void cpc_protocol_on_data(struct cpc_cport *cport, struct sk_buff *skb)
{
	struct cpc_header *cpc_hdr = (struct cpc_header *)skb->data;
	u8 seq = cpc_header_get_seq(cpc_hdr);
	bool expected_seq = false;

	mutex_lock(&cport->lock);

	expected_seq = seq == cport->tcb.ack;
	if (expected_seq)
		cport->tcb.ack++;
	else
		dev_warn_ratelimited(cpc_hd_dev(cport->cpc_hd),
				     "unexpected seq: %u, expected seq: %u\n", seq, cport->tcb.ack);

	mutex_unlock(&cport->lock);

	if (expected_seq) {
		skb_pull(skb, CPC_HEADER_SIZE);

		greybus_data_rcvd(cport->cpc_hd->gb_hd, cport->id, skb->data, skb->len);
	}
}
