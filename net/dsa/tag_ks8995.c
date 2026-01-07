// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Linus Walleij <linusw@kernel.org>
 */
#include <linux/etherdevice.h>
#include <linux/log2.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "tag.h"

/* The KS8995 Special Tag Packet ID (STPID)
 * pushes its tag in a way similar to a VLAN tag
 * -----------------------------------------------------------
 * | MAC DA | MAC SA | 2 bytes tag | 2 bytes TCI | EtherType |
 * -----------------------------------------------------------
 * The tag is: 0x8100 |= BIT(port), ports 0,1,2,3
 */

#define KS8995_NAME "ks8995"

#define KS8995_TAG_LEN 4

static struct sk_buff *ks8995_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u16 ks8995_tag;
	__be16 *p;
	u16 port;
	u16 tci;

	/* Prepare the special KS8995 tags */
	port = dsa_xmit_port_mask(skb, dev);
	/* The manual says to set this to the CPU port if no port is indicated */
	if (!port)
		port = BIT(5);

	ks8995_tag = ETH_P_8021Q | port;
	tci = port & VLAN_VID_MASK;

	/* Push in a tag between MAC and ethertype */
	netdev_dbg(dev, "egress packet tag: add tag %04x %04x to port %d\n",
		   ks8995_tag, tci, dp->index);

	skb_push(skb, KS8995_TAG_LEN);
	dsa_alloc_etype_header(skb, KS8995_TAG_LEN);

	p = dsa_etype_header_pos_tx(skb);
	p[0] = htons(ks8995_tag);
	p[1] = htons(tci);

	return skb;
}

static struct sk_buff *ks8995_rcv(struct sk_buff *skb, struct net_device *dev)
{
	unsigned int port;
	__be16 *p;
	u16 etype;
	u16 tci;

	if (unlikely(!pskb_may_pull(skb, KS8995_TAG_LEN))) {
		netdev_err(dev, "dropping packet, cannot pull\n");
		return NULL;
	}

	p = dsa_etype_header_pos_rx(skb);
	etype = ntohs(p[0]);

	if (etype == ETH_P_8021Q) {
		/* That's just an ordinary VLAN tag, pass through */
		return skb;
	}

	if ((etype & 0xFFF0U) != ETH_P_8021Q) {
		/* Not custom, just pass through */
		netdev_dbg(dev, "non-KS8995 ethertype 0x%04x\n", etype);
		return skb;
	}

	port = ilog2(etype & 0xF);
	tci = ntohs(p[1]);
	netdev_dbg(dev, "ingress packet tag: %04x %04x, port %d\n",
		   etype, tci, port);

	skb->dev = dsa_conduit_find_user(dev, 0, port);
	if (!skb->dev) {
		netdev_err(dev, "could not find user for port %d\n", port);
		return NULL;
	}

	/* Remove KS8995 tag and recalculate checksum */
	skb_pull_rcsum(skb, KS8995_TAG_LEN);

	dsa_strip_etype_header(skb, KS8995_TAG_LEN);

	dsa_default_offload_fwd_mark(skb);

	return skb;
}

static const struct dsa_device_ops ks8995_netdev_ops = {
	.name = KS8995_NAME,
	.proto	= DSA_TAG_PROTO_KS8995,
	.xmit = ks8995_xmit,
	.rcv = ks8995_rcv,
	.needed_headroom = KS8995_TAG_LEN,
};

MODULE_DESCRIPTION("DSA tag driver for Micrel KS8995 family of switches");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_KS8995, KS8995_NAME);

module_dsa_tag_driver(ks8995_netdev_ops);
