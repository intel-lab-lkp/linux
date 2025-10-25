// SPDX-License-Identifier: GPL-2.0+
/*
 * DSA driver Special Tag support for MaxLinear GSW1xx switch chips
 *
 * Copyright (C) 2025 Daniel Golle <daniel@makrotopia.org>
 * Copyright (C) 2023 - 2024 MaxLinear Inc.
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/dsa.h>

#include "tag.h"

/* To define the outgoing port and to discover the incoming port a special
 * tag is used by the GSW1xx.
 *
 *       Dest MAC       Src MAC    special TAG        EtherType
 * ...| 1 2 3 4 5 6 | 1 2 3 4 5 6 | 1 2 3 4 5 6 7 8 | 1 2 |...
 *                                |<--------------->|
 */

#define GSW1XX_TAG_NAME		"gsw1xx"

/* special tag in TX path header */
#define GSW1XX_TX_HEADER_LEN	8

/* Byte 0 = Ethertype byte 1 -> 0x88 */
/* Byte 1 = Ethertype byte 2 -> 0xC3*/

/* Byte 2 */
#define GSW1XX_TX_PORT_MAP_EN		BIT(7)
#define GSW1XX_TX_CLASS_EN		BIT(6)
#define GSW1XX_TX_TIME_STAMP_EN		BIT(5)
#define GSW1XX_TX_LRN_DIS		BIT(4)
#define GSW1XX_TX_CLASS_SHIFT		0
#define GSW1XX_TX_CLASS_MASK		GENMASK(3, 0)

/* Byte 3 */
#define GSW1XX_TX_PORT_MAP_LOW_SHIFT	0
#define GSW1XX_TX_PORT_MAP_LOW_MASK	GENMASK(7, 0)

/* Byte 4 */
#define GSW1XX_TX_PORT_MAP_HIGH_SHIFT	0
#define GSW1XX_TX_PORT_MAP_HIGH_MASK	GENMASK(7, 0)

#define GSW1XX_RX_HEADER_LEN		8

/* special tag in RX path header */
/* Byte 4 */
#define GSW1XX_RX_PORT_MAP_LOW_SHIFT	0
#define GSW1XX_RX_PORT_MAP_LOW_MASK	GENMASK(7, 0)

/* Byte 5 */
#define GSW1XX_RX_PORT_MAP_HIGH_SHIFT	0
#define GSW1XX_RX_PORT_MAP_HIGH_MASK	GENMASK(7, 0)

static struct sk_buff *gsw1xx_tag_xmit(struct sk_buff *skb,
				       struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u8 *gsw1xx_tag;

	/* provide additional space 'GSW1XX_TX_HEADER_LEN' bytes */
	skb_push(skb, GSW1XX_TX_HEADER_LEN);

	/* add space between MAC address and Ethertype */
	dsa_alloc_etype_header(skb, GSW1XX_TX_HEADER_LEN);

	/* special tag ingress */
	gsw1xx_tag = dsa_etype_header_pos_tx(skb);
	gsw1xx_tag[0] = 0x88;
	gsw1xx_tag[1] = 0xc3;
	gsw1xx_tag[2] = GSW1XX_TX_PORT_MAP_EN | GSW1XX_TX_LRN_DIS;
	gsw1xx_tag[3] = BIT(dp->index + GSW1XX_TX_PORT_MAP_LOW_SHIFT) &
			GSW1XX_TX_PORT_MAP_LOW_MASK;
	gsw1xx_tag[4] = 0;
	gsw1xx_tag[5] = 0;
	gsw1xx_tag[6] = 0;
	gsw1xx_tag[7] = 0;

	return skb;
}

static struct sk_buff *gsw1xx_tag_rcv(struct sk_buff *skb,
				      struct net_device *dev)
{
	int port;
	u8 *gsw1xx_tag;

	if (unlikely(!pskb_may_pull(skb, GSW1XX_RX_HEADER_LEN))) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet, cannot pull SKB\n");
		return NULL;
	}

	gsw1xx_tag = dsa_etype_header_pos_rx(skb);

	if (gsw1xx_tag[0] != 0x88 && gsw1xx_tag[1] != 0xc3) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet due to invalid special tag\n");
		dev_warn_ratelimited(&dev->dev,
				     "Tag: 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
				     gsw1xx_tag[0], gsw1xx_tag[1], gsw1xx_tag[2], gsw1xx_tag[3],
				     gsw1xx_tag[4], gsw1xx_tag[5], gsw1xx_tag[6], gsw1xx_tag[7]);
		return NULL;
	}

	/* Get source port information */
	port = (gsw1xx_tag[2] & GSW1XX_RX_PORT_MAP_LOW_MASK) >> GSW1XX_RX_PORT_MAP_LOW_SHIFT;
	skb->dev = dsa_conduit_find_user(dev, 0, port);
	if (!skb->dev) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet due to invalid source port\n");
		dev_warn_ratelimited(&dev->dev,
				     "Tag: 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
				     gsw1xx_tag[0], gsw1xx_tag[1], gsw1xx_tag[2], gsw1xx_tag[3],
				     gsw1xx_tag[4], gsw1xx_tag[5], gsw1xx_tag[6], gsw1xx_tag[7]);
		return NULL;
	}

	/* remove the GSW1xx special tag between MAC addresses and the current
	 * ethertype field.
	 */
	skb_pull_rcsum(skb, GSW1XX_RX_HEADER_LEN);
	dsa_strip_etype_header(skb, GSW1XX_RX_HEADER_LEN);

	return skb;
}

static const struct dsa_device_ops gsw1xx_netdev_ops = {
	.name = GSW1XX_TAG_NAME,
	.proto	= DSA_TAG_PROTO_MXL_GSW1XX,
	.xmit = gsw1xx_tag_xmit,
	.rcv = gsw1xx_tag_rcv,
	.needed_headroom = GSW1XX_RX_HEADER_LEN,
};

MODULE_DESCRIPTION("DSA tag driver for MaxLinear GSW1xx 8 byte protocol");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_MXL_GSW1XX, GSW1XX_TAG_NAME);

module_dsa_tag_driver(gsw1xx_netdev_ops);
