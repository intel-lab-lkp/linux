// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Joris Vaisvila <joey@tinyisr.com>
 * MT7628 switch tag support
 */

#include <linux/etherdevice.h>
#include <linux/dsa/8021q.h>
#include <linux/if_vlan.h>
#include <net/dsa.h>

#include "tag.h"

/*
 * The MT7628 tag is encoded in the VLAN TPID field.
 * On TX the lower 6 bits encode the destination port bitmask.
 * On RX the lower 3 bits encode the source port number.
 *
 * The switch can only use VLANs for forwarding control. VLAN-unaware bridges
 * are simulated using tag_8021q and double tagging, while VLAN-aware bridges
 * use the VLANs configured by the bridge directly.
 *
 * On egress, the tagger either adds a new MT7628 tag that contains the
 * standalone or bridge tag_8021q VLAN and destination port mask, or modifies
 * an existing VLAN tag to contain the destination port mask.
 *
 * On ingress, the VLAN tag is restored after stripping the MT7628 tag, if it
 * is not a tag_8021q VLAN.
 */

#define MT7628_TAG_NAME "mt7628"

#define MT7628_TAG_TX_PORT GENMASK(5, 0)
#define MT7628_TAG_RX_PORT GENMASK(2, 0)
#define MT7628_TAG_LEN 4

static struct sk_buff *mt7628_tag_xmit(struct sk_buff *skb,
				       struct net_device *dev)
{
	struct dsa_port *dp;
	u16 xmit_tpid;
	u16 xmit_vlan;
	__be16 *tag;

	xmit_tpid =
	    ETH_P_8021Q | FIELD_PREP(MT7628_TAG_TX_PORT,
				     dsa_xmit_port_mask(skb, dev));
	dp = dsa_user_to_port(dev);
	if (skb->offload_fwd_mark &&
	    br_vlan_enabled(dsa_port_bridge_dev_get(dp))) {
		/*
		 * On VLAN aware ports only modify the TPID to contain the
		 * MT7628 egress port metadata, instead of adding a new vlan tag
		 */
		tag = dsa_etype_header_pos_tx(skb);
		tag[0] = htons(xmit_tpid);
		return skb;
	}

	xmit_vlan = skb->offload_fwd_mark ?
	    dsa_tag_8021q_bridge_vid(dsa_port_bridge_num_get(dp)) :
	    dsa_tag_8021q_standalone_vid(dp);

	skb_push(skb, MT7628_TAG_LEN);
	dsa_alloc_etype_header(skb, MT7628_TAG_LEN);

	tag = dsa_etype_header_pos_tx(skb);

	tag[0] = htons(xmit_tpid);
	tag[1] = htons(xmit_vlan);

	return skb;
}

static struct sk_buff *mt7628_tag_rcv(struct sk_buff *skb,
				      struct net_device *dev)
{
	unsigned int source_port;
	bool is_dsa_8021q;
	__be16 *phdr;
	u16 tci, vid;

	if (unlikely(!pskb_may_pull(skb, MT7628_TAG_LEN))) {
		kfree_skb(skb);
		return NULL;
	}

	phdr = dsa_etype_header_pos_rx(skb);
	source_port = FIELD_GET(MT7628_TAG_RX_PORT, ntohs(*phdr));
	tci = ntohs(phdr[1]);
	vid = tci & VLAN_VID_MASK;
	is_dsa_8021q = vid_is_dsa_8021q(vid);

	/*
	 * The source port info is only encoded in the TPID field for packets
	 * where the VLAN tag is inserted by the PVID mechanism. With VLAN
	 * filtering enabled, VLAN-tagged ingress packets appear as if they're
	 * originating on port 0. Use the VID to identify the bridge port in
	 * this case.
	 */
	if (source_port == 0 && !is_dsa_8021q)
		skb->dev = dsa_find_designated_bridge_port_by_vid(dev, vid);
	else
		skb->dev = dsa_conduit_find_user(dev, 0, source_port);

	if (!skb->dev) {
		kfree_skb(skb);
		return NULL;
	}

	skb_pull_rcsum(skb, MT7628_TAG_LEN);
	dsa_strip_etype_header(skb, MT7628_TAG_LEN);
	dsa_default_offload_fwd_mark(skb);

	if (!is_dsa_8021q)
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), tci);

	return skb;
}

static const struct dsa_device_ops mt7628_tag_ops = {
	.name = MT7628_TAG_NAME,
	.proto = DSA_TAG_PROTO_MT7628,
	.xmit = mt7628_tag_xmit,
	.rcv = mt7628_tag_rcv,
	.needed_headroom = MT7628_TAG_LEN,
};

module_dsa_tag_driver(mt7628_tag_ops);

MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_MT7628, MT7628_TAG_NAME);
MODULE_DESCRIPTION("DSA tag driver for MT7628 switch");
MODULE_LICENSE("GPL");
