// SPDX-License-Identifier: GPL-2.0+
/*
 * Motorcomm YT921x Switch Extend CPU Port
 *
 * Copyright (c) 2025 David Yang <mmyangfl@gmail.com>
 *
 * +----+----+-------+-----+----+---------
 * | DA | SA | TagET | Tag | ET | Payload ...
 * +----+----+-------+-----+----+---------
 *   6    6      2      6    2       N
 *
 * Tag Ethertype: CPU_TAG_TPID_TPID (default: 0x9988)
 * Tag:
 *   2: Service VLAN Tag
 *   2: Rx Port
 *     15b: Rx Port Valid
 *     14b-11b: Rx Port
 *     10b-0b: Unknown value 0x80
 *   2: Tx Port(s)
 *     15b: Tx Port(s) Valid
 *     10b-0b: Tx Port(s) Mask
 */

#include <linux/etherdevice.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "tag.h"

#define YT921X_NAME	"yt921x"

#define YT921X_TAG_LEN	8

#define ETH_P_YT921X	0x9988

#define YT921X_TAG_PORT_EN		BIT(15)
#define YT921X_TAG_RX_PORT_M		GENMASK(14, 11)
#define YT921X_TAG_RX_CMD_M		GENMASK(10, 0)
#define  YT921X_TAG_RX_CMD(x)			FIELD_PREP(YT921X_TAG_RX_CMD_M, (x))
#define   YT921X_TAG_RX_CMD_UNK_NORMAL			0x80
#define YT921X_TAG_TX_PORTS_M		GENMASK(10, 0)
#define  YT921X_TAG_TX_PORTn(port)		BIT(port)

static struct sk_buff *
yt921x_tag_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct dsa_port *dp = dsa_user_to_port(netdev);
	unsigned int port = dp->index;
	struct dsa_port *partner;
	__be16 *tag;
	u16 tx;

	skb_push(skb, YT921X_TAG_LEN);
	dsa_alloc_etype_header(skb, YT921X_TAG_LEN);

	tag = dsa_etype_header_pos_tx(skb);

	/* We might use yt921x_priv::tag_eth_p, but
	 * 1. CPU_TAG_TPID could be configured anyway;
	 * 2. Are you using the right chip?
	 */
	tag[0] = htons(ETH_P_YT921X);
	/* Service VLAN tag not used */
	tag[1] = 0;
	tag[2] = 0;
	tx = YT921X_TAG_PORT_EN | YT921X_TAG_TX_PORTn(port);
	if (dp->hsr_dev)
		dsa_hsr_foreach_port(partner, dp->ds, dp->hsr_dev)
			tx |= YT921X_TAG_TX_PORTn(partner->index);
	tag[3] = htons(tx);

	/* Now tell the conduit network device about the desired output queue
	 * as well
	 */
	skb_set_queue_mapping(skb, port);

	return skb;
}

static struct sk_buff *
yt921x_tag_rcv(struct sk_buff *skb, struct net_device *netdev)
{
	unsigned int port;
	__be16 *tag;
	u16 rx;

	if (unlikely(!pskb_may_pull(skb, YT921X_TAG_LEN)))
		return NULL;

	tag = (__be16 *)skb->data;

	/* Locate which port this is coming from */
	rx = ntohs(tag[1]);
	if (unlikely((rx & YT921X_TAG_PORT_EN) == 0)) {
		netdev_err(netdev, "Unexpected rx tag 0x%04x\n", rx);
		return NULL;
	}

	port = FIELD_GET(YT921X_TAG_RX_PORT_M, rx);
	skb->dev = dsa_conduit_find_user(netdev, 0, port);
	if (unlikely(!skb->dev)) {
		netdev_err(netdev, "Cannot locate rx port %u\n", port);
		return NULL;
	}

	/* Remove YT921x tag and update checksum */
	skb_pull_rcsum(skb, YT921X_TAG_LEN);

	dsa_default_offload_fwd_mark(skb);

	dsa_strip_etype_header(skb, YT921X_TAG_LEN);

	return skb;
}

static const struct dsa_device_ops yt921x_netdev_ops = {
	.name	= YT921X_NAME,
	.proto	= DSA_TAG_PROTO_YT921X,
	.xmit	= yt921x_tag_xmit,
	.rcv	= yt921x_tag_rcv,
	.needed_headroom = YT921X_TAG_LEN,
};

MODULE_DESCRIPTION("DSA tag driver for Motorcomm YT921x switches");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_YT921X, YT921X_NAME);

module_dsa_tag_driver(yt921x_netdev_ops);
