// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/greybus.h>
#include <linux/list.h>
#include <linux/module.h>

#include "cpc.h"
#include "header.h"

struct cpc_endpoint *cpc_hd_get_endpoint(struct cpc_host_device *cpc_hd, u16 cport_id)
{
	struct cpc_endpoint *ep;

	for (int i = 0; i < ARRAY_SIZE(cpc_hd->endpoints); i++) {
		ep = cpc_hd->endpoints[i];
		if (ep && ep->id == cport_id)
			return ep;
	}

	return NULL;
}

void cpc_hd_rcvd(struct cpc_host_device *cpc_hd, struct cpc_header *hdr,
		 u8 *data, size_t length)
{
	enum cpc_frame_type type;
	struct cpc_endpoint *ep;
	u8 ep_id;

	cpc_header_get_type(hdr, &type);
	ep_id = cpc_header_get_ep_id(hdr);

	ep = cpc_hd_get_endpoint(cpc_hd, ep_id);
	if (!ep) {
		if (type != CPC_FRAME_TYPE_RST) {
			dev_dbg(&cpc_hd->gb_hd->dev, "ep%u not allocated (%d)\n", ep_id, type);
			cpc_protocol_send_rst(cpc_hd, ep_id);
		}
		return;
	}

	switch (type) {
	case CPC_FRAME_TYPE_DATA:
		cpc_protocol_on_data(ep, hdr, data, length);
		break;
	case CPC_FRAME_TYPE_SYN:
		cpc_protocol_on_syn(ep, hdr);
		break;
	case CPC_FRAME_TYPE_RST:
		dev_dbg(&cpc_hd->gb_hd->dev, "reset\n");
		cpc_protocol_on_rst(ep);
		break;
	}
}


/**
 * cpc_interface_send_frame() - Queue a socket buffer for transmission.
 * @intf: Interface to send SKB over.
 * @ops: SKB to send.
 *
 * Queue SKB in interface's transmit queue and signal the interface. Interface is expected to use
 * cpc_interface_dequeue() to get the next SKB to transmit.
 */
void cpc_hd_send_frame(struct cpc_host_device *cpc_hd, struct cpc_frame *frame)
{
	mutex_lock(&cpc_hd->lock);
	list_add_tail(&frame->txq_links, &cpc_hd->tx_queue);
	mutex_unlock(&cpc_hd->lock);

	cpc_hd->wake_tx(cpc_hd);
}

/**
 * cpc_interface_dequeue() - Get the next SKB that was queued for transmission.
 * @intf: Interface.
 *
 * Get an SKB that was previously queued by cpc_interface_send_frame().
 *
 * Return: An SKB, or %NULL if queue was empty.
 */
struct cpc_frame *cpc_hd_dequeue(struct cpc_host_device *cpc_hd)
{
	struct cpc_frame *f;

	mutex_lock(&cpc_hd->lock);
	f = list_first_entry_or_null(&cpc_hd->tx_queue, struct cpc_frame, txq_links);
	if (f)
		list_del(&f->txq_links);
	mutex_unlock(&cpc_hd->lock);

	return f;
}

/**
 * cpc_interface_tx_queue_empty() - Check if transmit queue is empty.
 * @intf: Interface.
 *
 * Return: True if transmit queue is empty, false otherwise.
 */
bool cpc_hd_tx_queue_empty(struct cpc_host_device *cpc_hd)
{
	bool empty;

	mutex_lock(&cpc_hd->lock);
	empty = list_empty(&cpc_hd->tx_queue);
	mutex_unlock(&cpc_hd->lock);

	return empty;
}
