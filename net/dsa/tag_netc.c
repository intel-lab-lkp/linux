// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025-2026 NXP
 */

#include <linux/dsa/tag_netc.h>

#include "tag.h"

#define NETC_NAME			"nxp_netc"

/* Forward NXP switch tag */
#define NETC_TAG_FORWARD		0

/* To_Port NXP switch tag */
#define NETC_TAG_TO_PORT		1
/* SubType0: No request to perform timestamping */
#define NETC_TAG_TP_SUBTYPE0		0

/* To_Host NXP switch tag */
#define NETC_TAG_TO_HOST		2
/* SubType0: frames redirected or copied to CPU port */
#define NETC_TAG_TH_SUBTYPE0		0
/* SubType1: frames redirected or copied to CPU port with timestamp */
#define NETC_TAG_TH_SUBTYPE1		1
/* SubType2: Transmit timestamp response (two-step timestamping) */
#define NETC_TAG_TH_SUBTYPE2		2

/* NETC switch tag lengths */
#define NETC_TAG_FORWARD_LEN		6
#define NETC_TAG_TP_SUBTYPE0_LEN	6
#define NETC_TAG_TH_SUBTYPE0_LEN	6
#define NETC_TAG_TH_SUBTYPE1_LEN	14
#define NETC_TAG_TH_SUBTYPE2_LEN	14
#define NETC_TAG_CMN_LEN		5

#define NETC_TAG_SUBTYPE		GENMASK(3, 0)
#define NETC_TAG_TYPE			GENMASK(7, 4)
#define NETC_TAG_QV			BIT(0)
#define NETC_TAG_IPV			GENMASK(4, 2)
#define NETC_TAG_SWITCH			GENMASK(2, 0)
#define NETC_TAG_PORT			GENMASK(7, 3)

struct netc_tag_cmn {
	__be16 tpid;
	u8 type;
	u8 qos;
	u8 switch_port;
} __packed;

static void netc_fill_common_tag(struct netc_tag_cmn *tag, u8 type,
				 u8 subtype, u8 sw_id, u8 port, u8 ipv)
{
	tag->tpid = htons(ETH_P_NXP_NETC);
	tag->type = FIELD_PREP(NETC_TAG_TYPE, type) |
		    FIELD_PREP(NETC_TAG_SUBTYPE, subtype);
	tag->qos = NETC_TAG_QV | FIELD_PREP(NETC_TAG_IPV, ipv);
	tag->switch_port = FIELD_PREP(NETC_TAG_SWITCH, sw_id) |
			   FIELD_PREP(NETC_TAG_PORT, port);
}

static void *netc_fill_common_tp_tag(struct sk_buff *skb,
				     struct net_device *ndev,
				     u8 subtype, int tag_len)
{
	struct dsa_port *dp = dsa_user_to_port(ndev);
	u16 queue = skb_get_queue_mapping(skb);
	u8 ipv = netdev_txq_to_tc(ndev, queue);
	void *tag;

	skb_push(skb, tag_len);
	dsa_alloc_etype_header(skb, tag_len);

	tag = dsa_etype_header_pos_tx(skb);
	memset(tag + NETC_TAG_CMN_LEN, 0, tag_len - NETC_TAG_CMN_LEN);
	netc_fill_common_tag(tag, NETC_TAG_TO_PORT, subtype,
			     dp->ds->index, dp->index, ipv);

	return tag;
}

static void netc_fill_tp_tag_subtype0(struct sk_buff *skb,
				      struct net_device *ndev)
{
	netc_fill_common_tp_tag(skb, ndev, NETC_TAG_TP_SUBTYPE0,
				NETC_TAG_TP_SUBTYPE0_LEN);
}

/* Currently only support To_Port tag, subtype 0 */
static struct sk_buff *netc_xmit(struct sk_buff *skb,
				 struct net_device *ndev)
{
	netc_fill_tp_tag_subtype0(skb, ndev);

	return skb;
}

static int netc_get_rx_tag_len(int rx_type)
{
	int type = FIELD_GET(NETC_TAG_TYPE, rx_type);

	if (type == NETC_TAG_TO_HOST) {
		u8 subtype = rx_type & NETC_TAG_SUBTYPE;

		if (subtype == NETC_TAG_TH_SUBTYPE1)
			return NETC_TAG_TH_SUBTYPE1_LEN;
		else if (subtype == NETC_TAG_TH_SUBTYPE2)
			return NETC_TAG_TH_SUBTYPE2_LEN;
		else
			return NETC_TAG_TH_SUBTYPE0_LEN;
	}

	return NETC_TAG_FORWARD_LEN;
}

static struct sk_buff *netc_rcv(struct sk_buff *skb,
				struct net_device *ndev)
{
	struct netc_tag_cmn *tag_cmn = dsa_etype_header_pos_rx(skb);
	int tag_len = netc_get_rx_tag_len(tag_cmn->type);
	int sw_id, port;

	if (ntohs(tag_cmn->tpid) != ETH_P_NXP_NETC) {
		dev_warn_ratelimited(&ndev->dev, "Unknown TPID 0x%04x\n",
				     ntohs(tag_cmn->tpid));

		return NULL;
	}

	if (tag_cmn->qos & NETC_TAG_QV)
		skb->priority = FIELD_GET(NETC_TAG_IPV, tag_cmn->qos);

	sw_id = NETC_TAG_SWITCH & tag_cmn->switch_port;
	/* ENETC VEPA switch ID (0) is not supported yet */
	if (!sw_id) {
		dev_warn_ratelimited(&ndev->dev,
				     "VEPA switch ID is not supported yet\n");

		return NULL;
	}

	port = FIELD_GET(NETC_TAG_PORT, tag_cmn->switch_port);
	skb->dev = dsa_conduit_find_user(ndev, sw_id, port);
	if (!skb->dev)
		return NULL;

	if (tag_cmn->type == NETC_TAG_FORWARD)
		dsa_default_offload_fwd_mark(skb);

	/* Remove Switch tag from the frame */
	skb_pull_rcsum(skb, tag_len);
	dsa_strip_etype_header(skb, tag_len);

	return skb;
}

static void netc_flow_dissect(const struct sk_buff *skb, __be16 *proto,
			      int *offset)
{
	struct netc_tag_cmn *tag_cmn = (struct netc_tag_cmn *)(skb->data - 2);
	int tag_len = netc_get_rx_tag_len(tag_cmn->type);

	*offset = tag_len;
	*proto = ((__be16 *)skb->data)[(tag_len / 2) - 1];
}

static const struct dsa_device_ops netc_netdev_ops = {
	.name			= NETC_NAME,
	.proto			= DSA_TAG_PROTO_NETC,
	.xmit			= netc_xmit,
	.rcv			= netc_rcv,
	.needed_headroom	= NETC_TAG_MAX_LEN,
	.flow_dissect		= netc_flow_dissect,
};

MODULE_DESCRIPTION("DSA tag driver for NXP NETC switch family");
MODULE_LICENSE("GPL");

MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_NETC, NETC_NAME);
module_dsa_tag_driver(netc_netdev_ops);
