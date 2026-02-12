// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/unaligned.h>
#include <linux/skbuff.h>

#include "cpc.h"
#include "host.h"

/**
 * cpc_cport_tcb_reset() - Reset cport's TCB to initial values.
 * @cport: cport pointer
 */
static void cpc_cport_tcb_reset(struct cpc_cport *cport)
{
	cport->tcb.ack = 0;
	cport->tcb.seq = 0;
}

/**
 * cpc_cport_alloc() - Allocate and initialize CPC cport.
 * @cport_id: cport ID.
 * @gfp_mask: GFP mask for allocation.
 *
 * Return: Pointer to allocated and initialized cpc_cport, or NULL on failure.
 */
struct cpc_cport *cpc_cport_alloc(u16 cport_id, gfp_t gfp_mask)
{
	struct cpc_cport *cport;

	cport = kzalloc(sizeof(*cport), gfp_mask);
	if (!cport)
		return NULL;

	cport->id = cport_id;
	cpc_cport_tcb_reset(cport);

	mutex_init(&cport->lock);

	return cport;
}

void cpc_cport_release(struct cpc_cport *cport)
{
	kfree(cport);
}

/**
 * cpc_cport_pack() - Pack CPort ID into Greybus Operation Message header.
 * @gb_hdr: Greybus operation message header.
 * @cport_id: CPort ID to pack.
 */
void cpc_cport_pack(struct gb_operation_msg_hdr *gb_hdr, u16 cport_id)
{
	put_unaligned_le16(cport_id, gb_hdr->pad);
}

/**
 * cpc_cport_unpack() - Unpack CPort ID from Greybus Operation Message header.
 * @gb_hdr: Greybus operation message header.
 *
 * Return: CPort ID packed in the header.
 */
u16 cpc_cport_unpack(struct gb_operation_msg_hdr *gb_hdr)
{
	u16 cport_id = get_unaligned_le16(gb_hdr->pad);

	// Clear padding bytes
	put_unaligned_le16(0, gb_hdr->pad);

	return cport_id;
}

/**
 * cpc_cport_transmit() - Transmit skb over cport.
 * @cport: cport.
 * @skb: skb to be transmitted.
 */
int cpc_cport_transmit(struct cpc_cport *cport, struct sk_buff *skb)
{
	struct cpc_host_device *cpc_hd = cport->cpc_hd;
	struct gb_operation_msg_hdr *gb_hdr;
	u8 ack;

	/* Inject cport ID in Greybus header */
	gb_hdr = (struct gb_operation_msg_hdr *)skb->data;
	cpc_cport_pack(gb_hdr, cport->id);

	mutex_lock(&cport->lock);

	CPC_SKB_CB(skb)->seq = cport->tcb.seq;
	CPC_SKB_CB(skb)->cpc_flags = CPC_SKB_FLAG_REQ_ACK;

	cport->tcb.seq++;
	ack = cport->tcb.ack;

	mutex_unlock(&cport->lock);

	cpc_protocol_prepare_header(skb, ack);

	return cpc_hd_send_skb(cpc_hd, skb);
}
