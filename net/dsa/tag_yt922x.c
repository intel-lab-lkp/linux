// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Motorcomm YT922x Switch Extended CPU Port Tagging
 *
 * Copyright (c) 2026 Kyle switch <kyle.switch@motor-comm.com>
 *
 */

#include <linux/etherdevice.h>

#include "tag.h"

#define YT922X_TAG_LEN	8

/*
 * To define the from cpu tag format 8 bytes:
 */
#define YT922X_TAG_NAME			"yt922x"
#define YT922X_TAG_PORTMASK_0		BIT(15)
#define YT922X_TAG_PORTMASK_M		GENMASK(8, 0)
#define  YT922X_TAG_PORTS(x)			FIELD_PREP(YT922X_TAG_PORTMASK_M, ((x) >> 0x1))
#define YT922X_TAG_FORCE_DST		BIT(9)
#define YT922X_TAG_PRIO_M		GENMASK(12, 10)
#define YT922X_TAG_PRIO_EN		BIT(13)
#define  YT922X_TAG_PRIO(x)			(FIELD_PREP(YT922X_TAG_PRIO_M, (x)) | YT922X_TAG_PRIO_EN)
#define YT922X_TAG_RX_PORT_M		GENMASK(5, 2)
#define YT922X_TAG_RX_PRIO_M		GENMASK(15, 13)

static struct sk_buff *
yt922x_tag_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct dsa_port *dp = dsa_user_to_port(netdev);
	__be16 *tag;
	u16 ctrl;

	skb_push(skb, YT922X_TAG_LEN);
	dsa_alloc_etype_header(skb, YT922X_TAG_LEN);
	tag = dsa_etype_header_pos_tx(skb);

	tag[0] = htons(ETH_P_YT921X);
	if (dp->index != 0) {
		/* Port index is not equal 0 in tag[1] */
		ctrl = YT922X_TAG_PRIO(skb->priority) | YT922X_TAG_FORCE_DST |
			YT922X_TAG_PORTS(dsa_xmit_port_mask(skb, netdev));
		tag[1] = htons(ctrl);
		tag[2] = 0;
	} else {
		/* Port 0 in bit15 in tag[2] */
		ctrl = YT922X_TAG_PRIO(skb->priority) | YT922X_TAG_FORCE_DST;
		tag[1] = htons(ctrl);
		ctrl = YT922X_TAG_PORTMASK_0;
		tag[2] = htons(ctrl);
	}
	tag[3] = 0;

	return skb;
}

static struct sk_buff *
yt922x_tag_rcv(struct sk_buff *skb, struct net_device *netdev)
{
	unsigned int port;
	__be16 *tag;
	u16 rx;

	if (unlikely(!pskb_may_pull(skb, YT922X_TAG_LEN))) {
		kfree_skb(skb);
		return NULL;
	}

	tag = dsa_etype_header_pos_rx(skb);

	if (unlikely(tag[0] != htons(ETH_P_YT921X))) {
		dev_warn_ratelimited(&netdev->dev,
				     "Unexpected EtherType 0x%04x\n",
				     ntohs(tag[0]));
		kfree_skb(skb);
		return NULL;
	}

	/* Locate which port this is coming from */
	rx = ntohs(tag[2]);
	port = FIELD_GET(YT922X_TAG_RX_PORT_M, rx);
	skb->dev = dsa_conduit_find_user(netdev, 0, port);
	if (unlikely(!skb->dev)) {
		dev_warn_ratelimited(&netdev->dev,
				     "Couldn't decode source port %u\n", port);
		kfree_skb(skb);
		return NULL;
	}

	/* Remove tag and update checksum */
	skb_pull_rcsum(skb, YT922X_TAG_LEN);
	dsa_strip_etype_header(skb, YT922X_TAG_LEN);

	return skb;
}

static const struct dsa_device_ops yt922x_netdev_ops = {
	.name   = YT922X_TAG_NAME,
	.proto  = DSA_TAG_PROTO_YT922X,
	.xmit   = yt922x_tag_xmit,
	.rcv    = yt922x_tag_rcv,
	.needed_headroom = YT922X_TAG_LEN,
};

MODULE_DESCRIPTION("DSA tag driver for Motorcomm YT922x switches");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_YT922X, YT922X_TAG_NAME);

module_dsa_tag_driver(yt922x_netdev_ops);
