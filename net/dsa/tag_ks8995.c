// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Linus Walleij <linusw@kernel.org>
 */
#include <linux/etherdevice.h>
#include <linux/log2.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "tag.h"

/* The KS8995 Special Tag Packet ID (STPID)
 * pushes its tag in a modified VLAN (802.1Q) tag.
 * -----------------------------------------------------------
 * | MAC DA | MAC SA | 2 bytes tag | 2 bytes TCI | EtherType |
 * -----------------------------------------------------------
 * The tag is: 0x8100 |= BIT(port), ports 0,1,2,3
 */

#define KS8995_NAME "ks8995"

#define KS8995M_STPID_STD	GENMASK(15, 4)
#define KS8995M_STPID_PORTMASK	GENMASK(3, 0)
#define KS8995M_STPID(portmask)	htons(ETH_P_8021Q | FIELD_PREP(KS8995M_STPID_PORTMASK, portmask))

static struct sk_buff *ks8995_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct vlan_ethhdr *hdr = vlan_eth_hdr(skb);
	bool have_hwaccel_tag = false;
	u16 tci = 0, portmask;

	/* Prepare the special KS8995 tags */
	portmask = dsa_xmit_port_mask(skb, dev);

	if (skb_vlan_tag_present(skb) && skb->vlan_proto == htons(ETH_P_8021Q)) {
		tci = skb_vlan_tag_get(skb);
		__vlan_hwaccel_clear_tag(skb);
		have_hwaccel_tag = true;
	}

	if (have_hwaccel_tag || hdr->h_vlan_proto != htons(ETH_P_8021Q)) {
		skb = vlan_insert_tag(skb, KS8995M_STPID(portmask), tci);
		if (!skb)
			return NULL;
		hdr = vlan_eth_hdr(skb);
		netdev_dbg(dev, "%s: inserted VLAN TAG %04x TCI %04x\n",
			   __func__, hdr->h_vlan_proto, hdr->h_vlan_TCI);
	} else {
		/* VLAN tag already exists in skb head, modify it in place */
		hdr = vlan_eth_hdr(skb);
		hdr->h_vlan_proto = KS8995M_STPID(portmask);
		hdr->h_vlan_TCI = htons(tci);
		netdev_dbg(dev, "%s: modified VLAN TAG %04x TCI %04x\n",
			   __func__, hdr->h_vlan_proto, hdr->h_vlan_TCI);
	}

	return skb;
}

static struct sk_buff *ks8995_rcv(struct sk_buff *skb, struct net_device *dev)
{
	int portmask;
	u16 etype;

	/* We are expecting all received packets to have a mangled VLAN
	 * TPID, so drop anything else. Because of the non-standard TPID,
	 * don't even bother looking for a tag in the hwaccel area.
	 *
	 * We have to inspect the ethertype directly because skb->protocol
	 * will contain garbage.
	 */
	etype = ntohs(*(u16 *)dsa_etype_header_pos_rx(skb));
	if ((etype & KS8995M_STPID_STD) != ETH_P_8021Q) {
		netdev_info(dev, "%s: dropped ethertype 0x%04x\n",
			    __func__, etype);
		return NULL;
	}
	netdev_dbg(dev, "%s: received ethertype %04x\n",
		   __func__, etype);

	/* Move the custom DSA+VLAN tag into the hwaccel area and strip
	 * it from the skb head
	 */
	skb = skb_vlan_untag(skb);
	if (!skb) {
		netdev_err(dev, "%s: unable to untag protocol %04x vlan protocol  %04x\n",
			   __func__, ntohs(skb->protocol), ntohs(skb->vlan_proto));
		return NULL;
	}

	portmask = FIELD_GET(KS8995M_STPID_PORTMASK, etype);
	netdev_dbg(dev, "%s: etype %04x portmask %04x (%d)\n",
		   __func__, etype, portmask, ilog2(portmask));
	skb->dev = dsa_conduit_find_user(dev, 0, ilog2(portmask));
	if (!skb->dev)
		return NULL;

	/* Preserve the VLAN tag if it contains a non-zero VID which is not
	 * identical to 0x001, or PCP, and restore its TPID to the standard
	 * value.
	 *
	 * If this is just an ordinary inbound package the datasheet claims
	 * it will "replace null VID with ingress port VID", which means
	 * VID set to 1: 0x8101 0001 for port 0 or 0x8102 0001 for port 1.
	 * So in the DSA driver we will set the default port VID to 0 so
	 * we can properly detect non-VLAN frames.
	 */
	if (!skb->vlan_tci) {
		netdev_dbg(dev, "%s: clear VLAN tag from frame\n", __func__);
		__vlan_hwaccel_clear_tag(skb);
	} else {
		skb->vlan_proto = htons(ETH_P_8021Q);
		netdev_dbg(dev, "%s: vlan_tci = 0x%04x VLAN frame\n",
			   __func__, skb->vlan_tci);
	}

	dsa_default_offload_fwd_mark(skb);

	return skb;
}

static const struct dsa_device_ops ks8995_netdev_ops = {
	.name = KS8995_NAME,
	.proto	= DSA_TAG_PROTO_KS8995,
	.xmit = ks8995_xmit,
	.rcv = ks8995_rcv,
	.needed_headroom = VLAN_HLEN,
};

MODULE_DESCRIPTION("DSA tag driver for Micrel KS8995 family of switches");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_KS8995, KS8995_NAME);

module_dsa_tag_driver(ks8995_netdev_ops);
