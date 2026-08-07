// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC tag protocol.
 */

#include "tag.h"

#define XLNX_TSN_NAME	"xlnx_tsn"

static struct sk_buff *xlnx_tsn_xmit(struct sk_buff *skb,
				     struct net_device *dev)
{
	return skb;
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
