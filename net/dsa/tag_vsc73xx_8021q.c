// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (C) 2022 Pawel Dembicki <paweldembicki@gmail.com>
 * Based on tag_sja1105.c:
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/dsa/8021q.h>

#include "tag.h"
#include "tag_8021q.h"

#define VSC73XX_8021Q_NAME "vsc73xx-8021q"

static struct sk_buff *vsc73xx_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct dsa_port *dp = dsa_slave_to_port(netdev);
	u16 queue_mapping = skb_get_queue_mapping(skb);
	u16 tx_vid = dsa_tag_8021q_standalone_vid(dp);
	u8 pcp;

	if (skb->offload_fwd_mark) {
		unsigned int bridge_num = dsa_port_bridge_num_get(dp);
		struct net_device *br = dsa_port_bridge_dev_get(dp);

		if (br_vlan_enabled(br))
			return skb;

		tx_vid = dsa_tag_8021q_bridge_vid(bridge_num);
	}

	pcp = netdev_txq_to_tc(netdev, queue_mapping);

	return dsa_8021q_xmit(skb, netdev, ETH_P_8021Q,
			      ((pcp << VLAN_PRIO_SHIFT) | tx_vid));
}

static void vsc73xx_vlan_rcv(struct sk_buff *skb, int *source_port,
			     int *switch_id, int *vbid, u16 *vid)
{
	if (vid_is_dsa_8021q(skb_vlan_tag_get(skb) & VLAN_VID_MASK))
		return dsa_8021q_rcv(skb, source_port, switch_id, vbid);

	/* Try our best with imprecise RX */
	*vid = skb_vlan_tag_get(skb) & VLAN_VID_MASK;
}

static struct sk_buff *vsc73xx_rcv(struct sk_buff *skb, struct net_device *netdev)
{
	int src_port = -1, switch_id = -1, vbid = -1;
	u16 vid;

	if (skb_vlan_tag_present(skb)) {
		/* Normal traffic path. */
		vsc73xx_vlan_rcv(skb, &src_port, &switch_id, &vbid, &vid);
	} else {
		netdev_warn(netdev, "Couldn't decode source port\n");
		return NULL;
	}

	if (vbid >= 1)
		skb->dev = dsa_tag_8021q_find_port_by_vbid(netdev, vbid);
	else if (src_port == -1 || switch_id == -1)
		skb->dev = dsa_find_designated_bridge_port_by_vid(netdev, vid);
	else
		skb->dev = dsa_master_find_slave(netdev, switch_id, src_port);
	if (!skb->dev) {
		netdev_warn(netdev, "Couldn't decode source port\n");
		return NULL;
	}

	dsa_default_offload_fwd_mark(skb);

	if (dsa_port_is_vlan_filtering(dsa_slave_to_port(skb->dev)) &&
	    eth_hdr(skb)->h_proto == htons(ETH_P_8021Q))
		__vlan_hwaccel_clear_tag(skb);

	return skb;
}

static const struct dsa_device_ops vsc73xx_8021q_netdev_ops = {
	.name			= VSC73XX_8021Q_NAME,
	.proto			= DSA_TAG_PROTO_VSC73XX_8021Q,
	.xmit			= vsc73xx_xmit,
	.rcv			= vsc73xx_rcv,
	.needed_headroom	= VLAN_HLEN,
	.promisc_on_master	= true,
};

MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_VSC73XX_8021Q, VSC73XX_8021Q_NAME);

module_dsa_tag_driver(vsc73xx_8021q_netdev_ops);
