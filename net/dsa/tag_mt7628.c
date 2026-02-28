// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Joris Vaisvila <joey@tinyisr.com>
 * MT7628 ralink special tag support
 */

#include <linux/etherdevice.h>
#include <linux/dsa/8021q.h>
#include <net/dsa.h>

#include "tag.h"

/* 
 * MT7628 uses the ralink special tag. It merges the VLAN TPID with source
 * port ID on RX and target port bitmap on TX.
 *
 * The switch forwarding is controlled with VLANs, so each port is put in a
 * standalone VLAN using tag_8021q. Double tag is enabled to simulate VLAN
 * unaware ports.
 *
 * A VLAN tag is constructed on egress to target the standalone VLAN and port.
 * The outer VLAN tag is parsed and removed on ingress.
 */

#define MT7628_TAG_NAME "tag_mt7628"

#define SPECIAL_HEADER_RECV_SOURCE_PORT_MASK GENMASK(2, 0)
#define SPECIAL_TAG_LEN 4

static struct sk_buff *dsa_user_special_xmit(struct sk_buff *skb,
					     struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u16 xmit_vlan = dsa_tag_8021q_standalone_vid(dp);
	u8 *special_tag;

	skb_push(skb, SPECIAL_TAG_LEN);
	dsa_alloc_etype_header(skb, SPECIAL_TAG_LEN);

	special_tag = dsa_etype_header_pos_tx(skb);

	special_tag[0] = ETH_P_8021Q >> 8;
	special_tag[1] = BIT(dp->index);

	special_tag[2] = xmit_vlan >> 8;
	special_tag[3] = xmit_vlan & 0xff;
	return skb;
}

static struct sk_buff *dsa_user_special_recv(struct sk_buff *skb,
					     struct net_device *dev)
{
	u16 hdr;
	int port;
	__be16 *phdr;

	if (unlikely(!pskb_may_pull(skb, SPECIAL_TAG_LEN)))
		return NULL;

	phdr = dsa_etype_header_pos_rx(skb);
	hdr = ntohs(*phdr);
	skb_pull_rcsum(skb, SPECIAL_TAG_LEN);
	dsa_strip_etype_header(skb, SPECIAL_TAG_LEN);

	port = hdr & SPECIAL_HEADER_RECV_SOURCE_PORT_MASK;

	skb->dev = dsa_conduit_find_user(dev, 0, port);
	if (!skb->dev)
		return NULL;
	dsa_default_offload_fwd_mark(skb);
	return skb;
}

static const struct dsa_device_ops mt7628_tag_ops = {
	.name = MT7628_TAG_NAME,
	.proto = DSA_TAG_PROTO_MT7628,
	.xmit = dsa_user_special_xmit,
	.rcv = dsa_user_special_recv,
	.needed_headroom = SPECIAL_TAG_LEN,
};
module_dsa_tag_driver(mt7628_tag_ops);

MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_MT7628, MT7628_TAG_NAME);
MODULE_DESCRIPTION("DSA tag driver for MT7628 switch");
MODULE_LICENSE("GPL v2");
