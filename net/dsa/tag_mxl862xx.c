// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DSA Special Tag for MaxLinear 862xx switch chips
 *
 * Copyright (C) 2025 Daniel Golle <daniel@makrotopia.org>
 * Copyright (C) 2024 MaxLinear Inc.
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/dsa.h>
#include "tag.h"

#define MXL862_NAME	"mxl862xx"

/* To define the outgoing port and to discover the incoming port a special
 * tag is used by the GSW1xx.
 *
 *       Dest MAC       Src MAC    special TAG        EtherType
 * ...| 1 2 3 4 5 6 | 1 2 3 4 5 6 | 1 2 3 4 5 6 7 8 | 1 2 |...
 *                                |<--------------->|
 */

#define MXL862_HEADER_LEN 8

/* Byte 7 */
#define MXL862_IGP_EGP GENMASK(3, 0)

static struct sk_buff *mxl862_tag_xmit(struct sk_buff *skb,
				       struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	struct dsa_port *cpu_dp = dp->cpu_dp;
	unsigned int cpu_port = cpu_dp->index + 1;
	unsigned int usr_port = dp->index + 1;
	__be16 *mxl862_tag;

	if (!skb)
		return skb;

	/* provide additional space 'MXL862_HEADER_LEN' bytes */
	skb_push(skb, MXL862_HEADER_LEN);

	/* shift MAC address to the beginnig of the enlarged buffer,
	 * releasing the space required for DSA tag (between MAC address and
	 * Ethertype)
	 */
	dsa_alloc_etype_header(skb, MXL862_HEADER_LEN);

	/* special tag ingress */
	mxl862_tag = dsa_etype_header_pos_tx(skb);
	mxl862_tag[0] = htons(ETH_P_MXLGSW);
	mxl862_tag[2] = htons(usr_port + 16 - cpu_port);
	mxl862_tag[3] = htons(FIELD_PREP(MXL862_IGP_EGP, cpu_port));

	return skb;
}

static struct sk_buff *mxl862_tag_rcv(struct sk_buff *skb,
				      struct net_device *dev)
{
	int port;
	__be16 *mxl862_tag;

	if (unlikely(!pskb_may_pull(skb, MXL862_HEADER_LEN))) {
		dev_warn_ratelimited(&dev->dev, "Cannot pull SKB, packet dropped\n");
		return NULL;
	}

	mxl862_tag = dsa_etype_header_pos_rx(skb);

	if (unlikely(mxl862_tag[0] != htons(ETH_P_MXLGSW))) {
		dev_warn_ratelimited(&dev->dev, "Invalid special tag marker, packet dropped\n");
		dev_warn_ratelimited(&dev->dev, "Rx Packet Tag: %8ph\n", mxl862_tag);
		return NULL;
	}

	/* Get source port information */
	port = FIELD_GET(MXL862_IGP_EGP, ntohs(mxl862_tag[3]));
	port = port - 1;
	skb->dev = dsa_conduit_find_user(dev, 0, port);
	if (!skb->dev) {
		dev_warn_ratelimited(&dev->dev, "Invalid source port, packet dropped\n");
		dev_warn_ratelimited(&dev->dev, "Rx Packet Tag: %8ph\n", mxl862_tag);
		return NULL;
	}

	/* remove the MxL862xx special tag between the MAC addresses and the
	 * current ethertype field.
	 */
	skb_pull_rcsum(skb, MXL862_HEADER_LEN);
	dsa_strip_etype_header(skb, MXL862_HEADER_LEN);

	return skb;
}

static const struct dsa_device_ops mxl862_netdev_ops = {
	.name = "mxl862",
	.proto = DSA_TAG_PROTO_MXL862,
	.xmit = mxl862_tag_xmit,
	.rcv = mxl862_tag_rcv,
	.needed_headroom = MXL862_HEADER_LEN,
};

MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_MXL862, MXL862_NAME);

module_dsa_tag_driver(mxl862_netdev_ops);
