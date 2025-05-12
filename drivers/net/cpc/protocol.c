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

static int __cpc_protocol_queue_tx_frame(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	struct cpc_interface *intf = ep->intf;
	struct sk_buff *cloned_skb;

	cloned_skb = skb_clone(skb, GFP_KERNEL);
	if (!cloned_skb)
		return -ENOMEM;

	cpc_interface_send_frame(intf, cloned_skb);

	return 0;
}

static void __cpc_protocol_process_pending_tx_frames(struct cpc_endpoint *ep)
{
	struct sk_buff *skb;
	int err;

	while ((skb = skb_dequeue(&ep->holding_queue))) {
		err = __cpc_protocol_queue_tx_frame(ep, skb);
		if (err < 0) {
			skb_queue_head(&ep->holding_queue, skb);
			return;
		}
	}
}

void cpc_protocol_on_data(struct cpc_endpoint *ep, struct sk_buff *skb)
{
	if (skb->len > CPC_HEADER_SIZE) {
		/* Strip header. */
		skb_pull(skb, CPC_HEADER_SIZE);

		if (ep->ops && ep->ops->rx)
			ep->ops->rx(ep, skb);
		else
			kfree_skb(skb);
	} else {
		kfree_skb(skb);
	}
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

	return 0;
}
