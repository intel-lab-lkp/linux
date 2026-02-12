// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include "cpc.h"
#include "host.h"

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

	return cport;
}

void cpc_cport_release(struct cpc_cport *cport)
{
	kfree(cport);
}

/**
 * cpc_cport_transmit() - Transmit skb over cport.
 * @cport: cport.
 * @skb: skb to be transmitted.
 */
int cpc_cport_transmit(struct cpc_cport *cport, struct sk_buff *skb)
{
	struct cpc_host_device *cpc_hd = cport->cpc_hd;

	return cpc_hd_send_skb(cpc_hd, skb);
}
