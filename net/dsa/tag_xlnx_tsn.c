// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC tag protocol.
 */

#include <linux/dsa/xlnx_tsn.h>
#include <linux/if_vlan.h>

#include "tag.h"

#define XLNX_TSN_NAME	"xlnx_tsn"

/* PTP frames must go directly into the egress MAC's hardware TX buffer,
 * not through the switch fabric or conduit DMA. Intercept here before
 * dsa_enqueue_skb() takes the frame and hand off our reference: ptp_tx()
 * consumes it, freeing the skb once the HW timestamp is read (or on
 * error).
 *
 * VLAN-tagged PTP is not supported. Match on the L2 ethertype and
 * skip any VLAN-tagged frame.
 */
static struct sk_buff *xlnx_tsn_xmit(struct sk_buff *skb,
				     struct net_device *dev)
{
	struct xlnx_tsn_tagger_data *tagger_data;
	struct dsa_port *dp;

	dp = dsa_user_to_port(dev);
	tagger_data = dp->ds->tagger_data;

	if (!tagger_data || !tagger_data->ptp_tx)
		return skb;

	if (eth_hdr(skb)->h_proto != htons(ETH_P_1588) ||
	    skb_vlan_tag_present(skb))
		return skb;

	tagger_data->ptp_tx(dp, skb);

	return NULL;
}

static struct sk_buff *xlnx_tsn_rcv(struct sk_buff *skb,
				    struct net_device *dev)
{
	kfree_skb(skb);
	return NULL;
}

static const struct dsa_device_ops xlnx_tsn_netdev_ops = {
	.name	= XLNX_TSN_NAME,
	.proto	= DSA_TAG_PROTO_XLNX_TSN,
	.xmit	= xlnx_tsn_xmit,
	.rcv	= xlnx_tsn_rcv,
};

module_dsa_tag_driver(xlnx_tsn_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_XLNX_TSN, XLNX_TSN_NAME);
MODULE_DESCRIPTION("DSA tag driver for AMD/Xilinx TSN Endpoint Ethernet MAC");
MODULE_LICENSE("GPL");
