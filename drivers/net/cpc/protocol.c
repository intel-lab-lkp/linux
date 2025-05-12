// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/mutex.h>
#include <linux/skbuff.h>

#include "cpc.h"
#include "header.h"
#include "interface.h"
#include "protocol.h"

int cpc_protocol_send_syn(struct cpc_endpoint *ep)
{
	struct cpc_header hdr;
	struct sk_buff *skb;
	int err;

	skb = cpc_skb_alloc(0, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	memset(&hdr, 0, sizeof(hdr));

	mutex_lock(&ep->tcb.lock);

	hdr.ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_SYN, true);
	hdr.ep_id = ep->id;
	hdr.recv_wnd = CPC_HEADER_MAX_RX_WINDOW;
	hdr.seq = ep->tcb.seq;
	hdr.syn.mtu = cpu_to_le16(U16_MAX);

	err = __cpc_protocol_write(ep, &hdr, skb);

	mutex_unlock(&ep->tcb.lock);

	if (err)
		kfree_skb(skb);

	return err;
}

static void __cpc_protocol_send_ack(struct cpc_endpoint *ep)
{
	struct cpc_header hdr;
	struct sk_buff *skb;

	skb = cpc_skb_alloc(0, GFP_KERNEL);
	if (!skb)
		return;

	memset(&hdr, 0, sizeof(hdr));
	hdr.ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_DATA, false);
	hdr.ep_id = ep->id;
	hdr.recv_wnd = CPC_HEADER_MAX_RX_WINDOW;
	hdr.ack = ep->tcb.ack;
	memcpy(skb_push(skb, sizeof(hdr)), &hdr, sizeof(hdr));

	cpc_interface_send_frame(ep->intf, skb);
}

/**
 * cpc_protocol_send_rst - send a RST frame
 * @intf: interface pointer
 * @ep_id: endpoint id
 */
void cpc_protocol_send_rst(struct cpc_interface *intf, u8 ep_id)
{
	struct cpc_header hdr = {
		.ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_RST, false),
		.ep_id = ep_id,
	};
	struct sk_buff *skb;

	skb = cpc_skb_alloc(0, GFP_KERNEL);
	if (!skb)
		return;

	memcpy(skb_push(skb, sizeof(hdr)), &hdr, sizeof(hdr));

	cpc_interface_send_frame(intf, skb);
}

static void cpc_protocol_on_tx_complete(struct sk_buff *skb)
{
	struct cpc_endpoint *ep = cpc_skb_get_ctx(skb);

	/*
	 * Increase the send_nxt sequence, this is used as the upper bound of sequence number that
	 * can be ACK'd by the remote.
	 */
	mutex_lock(&ep->tcb.lock);
	ep->tcb.send_nxt++;
	mutex_unlock(&ep->tcb.lock);
}

static int __cpc_protocol_queue_tx_frame(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	struct cpc_header *hdr = (struct cpc_header *)skb->data;
	struct cpc_interface *intf = ep->intf;
	struct sk_buff *cloned_skb;

	hdr->ack = ep->tcb.ack;

	cloned_skb = skb_clone(skb, GFP_KERNEL);
	if (!cloned_skb)
		return -ENOMEM;

	skb_queue_tail(&ep->pending_ack_queue, skb);

	cpc_skb_set_ctx(cloned_skb, cpc_protocol_on_tx_complete, ep);

	cpc_interface_send_frame(intf, cloned_skb);

	return 0;
}

static void __cpc_protocol_process_pending_tx_frames(struct cpc_endpoint *ep)
{
	struct sk_buff *skb;
	u8 window;
	int err;

	window = ep->tcb.send_wnd;

	while ((skb = skb_dequeue(&ep->holding_queue))) {
		if (!cpc_header_number_in_window(ep->tcb.send_una,
						 window,
						 cpc_header_get_seq(skb->data)))
			err = -ERANGE;
		else
			err = __cpc_protocol_queue_tx_frame(ep, skb);

		if (err < 0) {
			skb_queue_head(&ep->holding_queue, skb);
			return;
		}
	}
}

static void __cpc_protocol_receive_ack(struct cpc_endpoint *ep, u8 recv_wnd, u8 ack)
{
	struct sk_buff *skb;
	u8 acked_frames;

	ep->tcb.send_wnd = recv_wnd;

	skb = skb_peek(&ep->pending_ack_queue);
	if (!skb)
		goto out;

	/* Return if no frame to ACK. */
	if (!cpc_header_number_in_range(ep->tcb.send_una, ep->tcb.send_nxt, ack))
		goto out;

	/* Calculate how many frames will be ACK'd. */
	acked_frames = cpc_header_get_frames_acked_count(cpc_header_get_seq(skb->data),
							 ack,
							 skb_queue_len(&ep->pending_ack_queue));

	for (u8 i = 0; i < acked_frames; i++)
		kfree_skb(skb_dequeue(&ep->pending_ack_queue));

	ep->tcb.send_una += acked_frames;

out:
	__cpc_protocol_process_pending_tx_frames(ep);
}

static bool __cpc_protocol_is_syn_ack_valid(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	enum cpc_frame_type type;
	struct sk_buff *syn_skb;
	u8 syn_seq;
	u8 ack;

	/* Fetch the previously sent frame. */
	syn_skb = skb_peek(&ep->pending_ack_queue);
	if (!syn_skb) {
		dev_warn(&ep->dev, "cannot validate syn-ack, no frame was sent\n");
		return false;
	}

	cpc_header_get_type(syn_skb->data, &type);

	/* Verify if this frame is SYN. */
	if (type != CPC_FRAME_TYPE_SYN) {
		dev_warn(&ep->dev, "cannot validate syn-ack, no syn frame was sent (%d)\n", type);
		return false;
	}

	syn_seq = cpc_header_get_seq(syn_skb->data);
	ack = cpc_header_get_ack(skb->data);

	/* Validate received ACK with the SEQ used in the initial SYN. */
	if (!cpc_header_is_syn_ack_valid(syn_seq, ack)) {
		dev_warn(&ep->dev,
			 "syn-ack (%d) is not valid with previously sent syn-seq (%d)\n",
			 ack, syn_seq);
		return false;
	}

	return true;
}

void cpc_protocol_on_data(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	bool expected_seq;

	mutex_lock(&ep->tcb.lock);

	__cpc_protocol_receive_ack(ep,
				   cpc_header_get_recv_wnd(skb->data),
				   cpc_header_get_ack(skb->data));

	if (cpc_header_get_req_ack(skb->data)) {
		expected_seq = cpc_header_get_seq(skb->data) == ep->tcb.ack;
		if (expected_seq)
			ep->tcb.ack++;

		__cpc_protocol_send_ack(ep);

		if (!expected_seq) {
			dev_warn(&ep->dev,
				 "unexpected seq: %u, expected seq: %u\n",
				 cpc_header_get_seq(skb->data), ep->tcb.ack);
			mutex_unlock(&ep->tcb.lock);
			kfree_skb(skb);
			return;
		}
	}

	mutex_unlock(&ep->tcb.lock);

	if (skb->len > CPC_HEADER_SIZE) {
		/* Strip header. */
		skb_pull(skb, CPC_HEADER_SIZE);

		if (test_bit(CPC_ENDPOINT_UP, &ep->flags))
			ep->ops->rx(ep, skb);
		else
			kfree_skb(skb);
	} else {
		kfree_skb(skb);
	}
}

void cpc_protocol_on_syn(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	mutex_lock(&ep->tcb.lock);

	if (!__cpc_protocol_is_syn_ack_valid(ep, skb)) {
		cpc_protocol_send_rst(ep->intf, ep->id);

		goto out;
	}

	__cpc_protocol_receive_ack(ep,
				   cpc_header_get_recv_wnd(skb->data),
				   cpc_header_get_ack(skb->data));

	/* On SYN-ACK, the remote's SEQ becomes our starting ACK. */
	ep->tcb.ack = cpc_header_get_seq(skb->data);
	ep->tcb.mtu = cpc_header_get_mtu(skb->data);
	ep->tcb.ack++;

	complete(&ep->conn);

	__cpc_protocol_send_ack(ep);

	set_bit(CPC_ENDPOINT_UP, &ep->flags);
	complete(&ep->conn);

out:
	mutex_unlock(&ep->tcb.lock);

	kfree_skb(skb);
}

void cpc_protocol_on_rst(struct cpc_endpoint *ep)
{
	__cpc_endpoint_disconnect(ep, false);
}

/**
 * __cpc_protocol_write() - Write a frame.
 * @ep: Endpoint handle.
 * @hdr: Header to write.
 * @skb: Payload to write.
 *
 * Context: Expect endpoint's lock to be held.
 *
 * Return: 0 on success, otherwise a negative error code.
 */
int __cpc_protocol_write(struct cpc_endpoint *ep,
			 struct cpc_header *hdr,
			 struct sk_buff *skb)
{
	memcpy(skb_push(skb, sizeof(*hdr)), hdr, sizeof(*hdr));

	skb_queue_tail(&ep->holding_queue, skb);

	__cpc_protocol_process_pending_tx_frames(ep);

	ep->tcb.seq++;

	return 0;
}
