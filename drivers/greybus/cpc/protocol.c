// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/greybus.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>

#include "cpc.h"
#include "header.h"

int cpc_protocol_send_syn(struct cpc_endpoint *ep)
{
	struct cpc_frame *frame;
	struct cpc_header *hdr;

	frame = cpc_frame_alloc(NULL, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	hdr = &frame->header;
	memset(hdr, 0, sizeof(*hdr));

	mutex_lock(&ep->lock);

	hdr->ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_SYN, true);
	hdr->ep_id = ep->id;
	hdr->recv_wnd = CPC_HEADER_MAX_RX_WINDOW;
	hdr->seq = ep->tcb.seq;
	hdr->syn.mtu = cpu_to_le16(U16_MAX);

	cpc_hd_send_frame(ep->cpc_hd, frame);

	mutex_unlock(&ep->lock);

	return 0;
}

static void __cpc_protocol_send_ack(struct cpc_endpoint *ep)
{
	struct cpc_frame *frame;
	struct cpc_header *hdr;

	frame = cpc_frame_alloc(NULL, GFP_KERNEL);
	if (!frame)
		return;

	hdr = &frame->header;

	memset(hdr, 0, sizeof(*hdr));
	hdr->ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_DATA, false);
	hdr->ep_id = ep->id;
	hdr->recv_wnd = CPC_HEADER_MAX_RX_WINDOW;
	hdr->ack = ep->tcb.ack;

	cpc_hd_send_frame(ep->cpc_hd, frame);
}

/**
 * cpc_protocol_send_rst - send a RST frame
 * @cpc_hd: host device pointer
 * @ep_id: endpoint id
 */
void cpc_protocol_send_rst(struct cpc_host_device *cpc_hd, u8 ep_id)
{
	struct cpc_frame *frame;
	struct cpc_header *hdr;

	frame = cpc_frame_alloc(NULL, GFP_KERNEL);
	if (!frame)
		return;

	hdr = &frame->header;
	memset(hdr, 0, sizeof(*hdr));
	hdr->ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_RST, false);
	hdr->ep_id = ep_id;

	cpc_hd_send_frame(cpc_hd, frame);
}

static int __cpc_protocol_queue_tx_frame(struct cpc_endpoint *ep, struct cpc_frame *frame)
{
	frame->header.ack = ep->tcb.ack;

	list_add_tail(&frame->links, &ep->pending_ack_queue);

	cpc_hd_send_frame(ep->cpc_hd, frame);

	return 0;
}

static void __cpc_protocol_process_pending_tx_frames(struct cpc_endpoint *ep)
{
	struct cpc_frame *frame;
	u8 window;
	int err;

	window = ep->tcb.send_wnd;

	while ((frame = list_first_entry_or_null(&ep->holding_queue,
						 struct cpc_frame,
						 links))) {
		if (!cpc_header_number_in_window(ep->tcb.send_una,
						 window,
						 cpc_header_get_seq(&frame->header)))
			return;

		list_del(&frame->links);

		err = __cpc_protocol_queue_tx_frame(ep, frame);
		if (err < 0) {
			list_add(&frame->links, &ep->holding_queue);
			return;
		}
	}
}

static void __cpc_protocol_receive_ack(struct cpc_endpoint *ep, u8 recv_wnd, u8 ack)
{
	struct cpc_frame *frame;
	u8 acked_frames;

	ep->tcb.send_wnd = recv_wnd;

	frame = list_first_entry_or_null(&ep->pending_ack_queue, struct cpc_frame, links);
	if (!frame)
		goto out;

	/* Return if no frame to ACK. */
	if (!cpc_header_number_in_range(ep->tcb.send_una, ep->tcb.send_nxt, ack))
		goto out;

	/* Calculate how many frames will be ACK'd. */
	acked_frames = cpc_header_get_frames_acked_count(cpc_header_get_seq(&frame->header), ack);

	for (u8 i = 0; i < acked_frames; i++) {
		frame = list_first_entry_or_null(&ep->pending_ack_queue, struct cpc_frame, links);
		if (!frame) {
			dev_err_ratelimited(&ep->cpc_hd->gb_hd->dev, "pending ack queue shorter than expected");
			break;
		}

		list_del(&frame->links);
		cpc_frame_free(frame);
	}

	ep->tcb.send_una += acked_frames;

out:
	__cpc_protocol_process_pending_tx_frames(ep);
}

static bool __cpc_protocol_is_syn_ack_valid(struct cpc_endpoint *ep, struct cpc_header *hdr)
{
	struct cpc_frame *syn_frame;
	enum cpc_frame_type type;
	u8 syn_seq;
	u8 ack;

	/* Fetch the previously sent frame. */
	syn_frame = list_first_entry_or_null(&ep->pending_ack_queue, struct cpc_frame, links);
	if (!syn_frame) {
		dev_warn(&ep->cpc_hd->gb_hd->dev, "cannot validate syn-ack, no frame was sent\n");
		return false;
	}

	cpc_header_get_type(&syn_frame->header, &type);

	/* Verify if this frame is SYN. */
	if (type != CPC_FRAME_TYPE_SYN) {
		dev_warn(&ep->cpc_hd->gb_hd->dev,
			 "cannot validate syn-ack, no syn frame was sent (%d)\n", type);
		return false;
	}

	syn_seq = cpc_header_get_seq(&syn_frame->header);
	ack = cpc_header_get_ack(hdr);

	/* Validate received ACK with the SEQ used in the initial SYN. */
	if (!cpc_header_is_syn_ack_valid(syn_seq, ack)) {
		dev_warn(&ep->cpc_hd->gb_hd->dev,
			 "syn-ack (%d) is not valid with previously sent syn-seq (%d)\n",
			 ack, syn_seq);
		return false;
	}

	return true;
}

void cpc_protocol_on_data(struct cpc_endpoint *ep, struct cpc_header *hdr,
			  u8 *data, size_t length)
{
	bool expected_seq;

	mutex_lock(&ep->lock);

	__cpc_protocol_receive_ack(ep,
				   cpc_header_get_recv_wnd(hdr),
				   cpc_header_get_ack(hdr));

	if (cpc_header_get_req_ack(hdr)) {
		expected_seq = cpc_header_get_seq(hdr) == ep->tcb.ack;
		if (expected_seq)
			ep->tcb.ack++;

		__cpc_protocol_send_ack(ep);

		if (!expected_seq)
			dev_warn(&ep->cpc_hd->gb_hd->dev,
				 "unexpected seq: %u, expected seq: %u\n",
				 cpc_header_get_seq(hdr), ep->tcb.ack);
	}

	mutex_unlock(&ep->lock);

	if (data) {
		if (expected_seq)
			greybus_data_rcvd(ep->cpc_hd->gb_hd, ep->id, data, length);
		else
			kfree(data);
	}
}

void cpc_protocol_on_syn(struct cpc_endpoint *ep, struct cpc_header *hdr)
{
	mutex_lock(&ep->lock);

	if (!__cpc_protocol_is_syn_ack_valid(ep, hdr)) {
		cpc_protocol_send_rst(ep->cpc_hd, ep->id);
		goto out;
	}

	__cpc_protocol_receive_ack(ep,
				   cpc_header_get_recv_wnd(hdr),
				   cpc_header_get_ack(hdr));

	/* On SYN-ACK, the remote's SEQ becomes our starting ACK. */
	ep->tcb.ack = cpc_header_get_seq(hdr);
	ep->tcb.mtu = cpc_header_get_mtu(hdr);
	ep->tcb.ack++;

	__cpc_protocol_send_ack(ep);

	complete(&ep->completion);

out:
	mutex_unlock(&ep->lock);
}

void cpc_protocol_on_rst(struct cpc_endpoint *ep)
{
	// To be implemented when connection mechanism are restored
}

/**
 * __cpc_protocol_write() - Write a frame.
 * @ep: Endpoint handle.
 * @frame: Frame to write.
 *
 * Context: Expect endpoint's lock to be held.
 *
 * Return: 0 on success, otherwise a negative error code.
 */
int __cpc_protocol_write(struct cpc_endpoint *ep, struct cpc_frame *frame)
{
	list_add_tail(&frame->links, &ep->holding_queue);

	__cpc_protocol_process_pending_tx_frames(ep);

	ep->tcb.seq++;

	return 0;
}
