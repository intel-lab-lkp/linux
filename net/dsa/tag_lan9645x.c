// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2026 Microchip Technology Inc.
 */

#include <linux/dsa/lan9645x.h>
#include <linux/if_ether.h>
#include <linux/if_hsr.h>
#include <linux/if_vlan.h>
#include <linux/igmp.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/addrconf.h>
#include <net/dsa.h>

#include "tag.h"

#define LAN9645X_NAME "lan9645x"

static struct sk_buff *lan9645x_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct dsa_port *dp = dsa_user_to_port(ndev);
	struct dsa_switch *ds = dp->ds;
	u32 cpu_port = ds->num_ports;
	u32 vlan_tci, tag_type;
	u32 qos_class;
	void *ifh;

	lan9645x_xmit_get_vlan_info(skb, dsa_port_bridge_dev_get(dp), &vlan_tci,
				    &tag_type);

	qos_class = netdev_get_num_tc(ndev) ?
		netdev_get_prio_tc_map(ndev, skb->priority) :
		skb->priority;

	/* Make room for IFH */
	ifh = skb_push(skb, LAN9645X_IFH_LEN);
	memset(ifh, 0, LAN9645X_IFH_LEN);

	LAN9645X_IFH_SET(ifh, IFH_BYPASS, 1);
	LAN9645X_IFH_SET(ifh, IFH_SRCPORT, cpu_port);
	LAN9645X_IFH_SET(ifh, IFH_QOS_CLASS, qos_class);
	LAN9645X_IFH_SET(ifh, IFH_TCI, vlan_tci);
	LAN9645X_IFH_SET(ifh, IFH_TAG_TYPE, tag_type);
	LAN9645X_IFH_SET(ifh, IFH_DSTS, BIT(dp->index));

	return skb;
}

static struct sk_buff *lan9645x_rcv(struct sk_buff *skb,
				    struct net_device *ndev)
{
	u32 src_port, qos_class, vlan_tci, tag_type, popcnt, etype_ofs;
	u8 *orig_skb_data = skb->data;
	struct dsa_port *dp;
	u32 ifh_gap_len = 0;
	u16 vlan_tpid;
	u8 *ifh;

	/* DSA master already consumed DMAC,SMAC,ETYPE from long prefix. Go back
	 * to beginning of frame.
	 */
	skb_push(skb, ETH_HLEN);
	/* IFH starts after our long prefix */
	ifh = skb_pull(skb, LAN9645X_LONG_PREFIX_LEN);

	src_port = LAN9645X_IFH_GET(ifh, IFH_SRCPORT);
	qos_class = LAN9645X_IFH_GET(ifh, IFH_QOS_CLASS);
	tag_type = LAN9645X_IFH_GET(ifh, IFH_TAG_TYPE);
	vlan_tci = LAN9645X_IFH_GET(ifh, IFH_TCI);
	popcnt = LAN9645X_IFH_GET(ifh, IFH_POP_CNT);
	etype_ofs = LAN9645X_IFH_GET(ifh, IFH_ETYPE_OFS);

	/* Set skb->data at start of real header
	 *
	 * Since REW_PORT_NO_REWRITE=0 is required on the NPI port, we need to
	 * account for any tags popped by the hardware, as that will leave a gap
	 * between the IFH and DMAC.
	 */
	if (popcnt == 0 && etype_ofs == 0)
		ifh_gap_len = 2 * VLAN_HLEN;
	else if (popcnt == 3)
		ifh_gap_len = VLAN_HLEN;

	skb_pull(skb, LAN9645X_IFH_LEN + ifh_gap_len);
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, ETH_HLEN);
	skb_reset_mac_len(skb);

	/* Reset skb->data past the actual ethernet header. */
	skb_pull(skb, ETH_HLEN);
	skb_postpull_rcsum(skb, orig_skb_data,
			   LAN9645X_TOTAL_TAG_LEN + ifh_gap_len);

	skb->dev = dsa_conduit_find_user(ndev, 0, src_port);
	if (WARN_ON_ONCE(!skb->dev)) {
		/* This should never happen since we have disabled reflection
		 * back to CPU_PORT.
		 */
		return NULL;
	}

	dsa_default_offload_fwd_mark(skb);

	skb->priority = qos_class;

	/* While we have REW_PORT_NO_REWRITE=0 on the NPI port, we still disable
	 * port VLAN tagging with REW_TAG_CFG. Any classified VID, different
	 * from a VID in the frame, will not be written to the frame, but is
	 * only communicated via the IFH. So for VLAN-aware ports we add the IFH
	 * vlan to the skb.
	 */
	dp = dsa_user_to_port(skb->dev);
	vlan_tpid = tag_type ? ETH_P_8021AD : ETH_P_8021Q;

	if (dsa_port_is_vlan_filtering(dp) &&
	    eth_hdr(skb)->h_proto == htons(vlan_tpid)) {
		u16 dummy_vlan_tci;

		skb_push_rcsum(skb, ETH_HLEN);
		__skb_vlan_pop(skb, &dummy_vlan_tci);
		skb_pull_rcsum(skb, ETH_HLEN);
		__vlan_hwaccel_put_tag(skb, htons(vlan_tpid), vlan_tci);
	}

	return skb;
}

static const struct dsa_device_ops lan9645x_netdev_ops = {
	.name = LAN9645X_NAME,
	.proto = DSA_TAG_PROTO_LAN9645X,
	.xmit = lan9645x_xmit,
	.rcv = lan9645x_rcv,
	.needed_headroom = LAN9645X_TOTAL_TAG_LEN,
	.promisc_on_conduit = false,
};

MODULE_DESCRIPTION("DSA tag driver for LAN9645x family of switches, using NPI port");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_LAN9645X, LAN9645X_NAME);

module_dsa_tag_driver(lan9645x_netdev_ops);
