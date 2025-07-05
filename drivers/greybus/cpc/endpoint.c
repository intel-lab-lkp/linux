// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/greybus.h>

#include "cpc.h"

/**
 * cpc_endpoint_write - Write a DATA frame.
 * @ep: Endpoint handle.
 * @frame: Frame to send.
 *
 * @return: 0 on success, otherwise a negative error code.
 */
int cpc_endpoint_frame_send(struct cpc_endpoint *ep, struct cpc_frame *frame)
{
	struct cpc_header *hdr = &frame->header;
	size_t cpc_payload_sz = 0;
	int err;

	if (frame->message) {
		cpc_payload_sz += sizeof(struct gb_operation_msg_hdr);
		cpc_payload_sz += frame->message->payload_size;
	}

	mutex_lock(&ep->lock);

	if (cpc_payload_sz > ep->tcb.mtu) {
		err = -EINVAL;
		goto out;
	}

	memset(hdr, 0, sizeof(*hdr));
	hdr->ctrl = cpc_header_get_ctrl(CPC_FRAME_TYPE_DATA, true);
	hdr->ep_id = ep->id;
	hdr->recv_wnd = CPC_HEADER_MAX_RX_WINDOW;
	hdr->seq = ep->tcb.seq;
	hdr->dat.payload_len = cpc_payload_sz;

	frame->ep = ep;

	err = __cpc_protocol_write(ep, frame);

out:
	mutex_unlock(&ep->lock);

	return err;
}

void cpc_frame_sent(struct cpc_frame *frame, int status)
{
	struct cpc_endpoint *ep = frame->ep;
	struct gb_host_device *gb_hd = ep->cpc_hd->gb_hd;

	/* There is no Greybus payload, this frame is purely CPC */
	if (!frame->message)
		return;

	/*
	 * Increase the send_nxt sequence, this is used as the upper bound of sequence number that
	 * can be ACK'd by the remote. Only increase if sent successfully.
	 */
	if (!status) {
		mutex_lock(&ep->lock);
		ep->tcb.send_nxt++;
		mutex_unlock(&ep->lock);
	}

	if (!frame->cancelled)
		greybus_message_sent(gb_hd, frame->message, status);

	kfree(frame);
}

/**
 * cpc_endpoint_tcb_reset() - Reset endpoint's TCB to initial values.
 * @ep: endpoint pointer
 */
static void cpc_endpoint_tcb_reset(struct cpc_endpoint *ep)
{
	ep->tcb.seq = ep->id;
	ep->tcb.ack = 0;
	ep->tcb.mtu = 0;
	ep->tcb.send_nxt = ep->id;
	ep->tcb.send_una = ep->id;
	ep->tcb.send_wnd = 1;
}

/**
 * cpc_endpoint_alloc() - Allocate and initialize CPC endpoint.
 * @ep_id: Endpoint ID.
 * @gfp_mask: GFP mask for allocation.
 *
 * Return: Pointer to allocated and initialized cpc_endpoint, or NULL on failure.
 */
struct cpc_endpoint *cpc_endpoint_alloc(u16 ep_id, gfp_t gfp_mask)
{
	struct cpc_endpoint *ep;

	ep = kzalloc(sizeof(*ep), gfp_mask);
	if (!ep)
		return NULL;

	ep->id = ep_id;
	INIT_LIST_HEAD(&ep->holding_queue);
	INIT_LIST_HEAD(&ep->pending_ack_queue);

	mutex_init(&ep->lock);
	cpc_endpoint_tcb_reset(ep);
	init_completion(&ep->completion);

	return ep;
}

void cpc_endpoint_release(struct cpc_endpoint *ep)
{
	kfree(ep);
}

int cpc_endpoint_connect(struct cpc_endpoint *ep)
{
	int ret;

	ret = cpc_protocol_send_syn(ep);
	if (ret)
		return ret;

	return wait_for_completion_interruptible(&ep->completion);
}

int cpc_endpoint_disconnect(struct cpc_endpoint *ep)
{
	cpc_protocol_send_rst(ep->cpc_hd, ep->id);

	return 0;
}

struct cpc_frame *cpc_frame_alloc(struct gb_message *message, gfp_t gfp_mask)
{
	struct cpc_frame *frame;

	frame = kzalloc(sizeof(*frame), gfp_mask);
	if (!frame)
		return NULL;

	frame->message = message;
	INIT_LIST_HEAD(&frame->links);
	INIT_LIST_HEAD(&frame->txq_links);

	return frame;
}

void cpc_frame_free(struct cpc_frame *frame)
{
	kfree(frame);
}
