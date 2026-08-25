// SPDX-License-Identifier: GPL-2.0-only

#include "netlink.h"
#include "common.h"

struct intf_caps_req_info {
	struct ethnl_req_info base;
};

struct intf_caps_reply_data {
	struct ethnl_reply_data base;
	struct ethtool_intf_caps caps;
};

#define INTF_CAPS_REPDATA(__reply_base) \
	container_of(__reply_base, struct intf_caps_reply_data, base)

const struct nla_policy ethnl_intf_caps_get_policy[ETHTOOL_A_INTF_CAPS_HEADER + 1] = {
	[ETHTOOL_A_INTF_CAPS_HEADER] =
		NLA_POLICY_NESTED(ethnl_header_policy),
};

static int intf_caps_reply_size(const struct ethnl_req_info *req_base,
				const struct ethnl_reply_data *reply_base)
{
	const struct intf_caps_reply_data *data = INTF_CAPS_REPDATA(reply_base);
	int len = 0;
	u32 i;

	for (i = 0; i < data->caps.num_blocks; i++) {
		len += nla_total_size(0);		/* nested block */
		len += nla_total_size(sizeof(u32));	/* id */
		len += nla_total_size(sizeof(u32));	/* component */
		len += nla_total_size(sizeof(u32));	/* sublayer */
		len += nla_total_size(sizeof(u32));	/* instance */
		len += nla_total_size(INTF_BLOCK_NAME_LEN); /* name */
		len += nla_total_size(sizeof(u8));	/* depth */
		len += nla_total_size(sizeof(u32));	/* lanes */
		len += nla_total_size(sizeof(u32));	/* loopback_supported */
		len += nla_total_size(sizeof(u32));	/* tx_patterns */
		len += nla_total_size(sizeof(u32));	/* rx_patterns */
		len += nla_total_size(0);		/* error_inject (flag) */
		len += nla_total_size(0);		/* bert (flag) */
	}

	/* outer BLOCKS nest */
	len += nla_total_size(0);

	return len;
}

static int intf_caps_prepare_data(const struct ethnl_req_info *req_base,
				  struct ethnl_reply_data *reply_base,
				  const struct genl_info *info)
{
	struct intf_caps_reply_data *data = INTF_CAPS_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;

	if (!dev->ethtool_ops->get_intf_caps)
		return -EOPNOTSUPP;

	return dev->ethtool_ops->get_intf_caps(dev, &data->caps);
}

static int intf_caps_fill_reply(struct sk_buff *skb,
				const struct ethnl_req_info *req_base,
				const struct ethnl_reply_data *reply_base)
{
	const struct intf_caps_reply_data *data = INTF_CAPS_REPDATA(reply_base);
	struct nlattr *blocks_attr;
	u32 i;

	blocks_attr = nla_nest_start(skb, ETHTOOL_A_INTF_CAPS_BLOCKS);
	if (!blocks_attr)
		return -EMSGSIZE;

	for (i = 0; i < data->caps.num_blocks; i++) {
		const struct ethtool_intf_block *b = &data->caps.blocks[i];
		struct nlattr *block_attr;

		block_attr = nla_nest_start(skb, 0);
		if (!block_attr)
			goto nla_put_failure;

		if (nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_ID, b->id) ||
		    nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_COMPONENT,
				b->component) ||
		    nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_SUBLAYER,
				b->sublayer) ||
		    nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_INSTANCE,
				b->instance) ||
		    nla_put_string(skb, ETHTOOL_A_INTF_BLOCK_NAME, b->name) ||
		    nla_put_u8(skb, ETHTOOL_A_INTF_BLOCK_DEPTH, b->depth) ||
		    nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_LANES, b->lanes) ||
		    nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_LOOPBACK_SUPPORTED,
				b->loopback_supported) ||
		    nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_TX_PATTERNS,
				b->supported_tx_patterns) ||
		    nla_put_u32(skb, ETHTOOL_A_INTF_BLOCK_RX_PATTERNS,
				b->supported_rx_patterns))
			goto nla_put_failure;
		if (b->error_inject_supported &&
		    nla_put_flag(skb, ETHTOOL_A_INTF_BLOCK_ERROR_INJECT))
			goto nla_put_failure;
		if (b->bert_supported &&
		    nla_put_flag(skb, ETHTOOL_A_INTF_BLOCK_BERT))
			goto nla_put_failure;
			goto nla_put_failure;

		nla_nest_end(skb, block_attr);
	}

	nla_nest_end(skb, blocks_attr);
	return 0;

nla_put_failure:
	nla_nest_cancel(skb, blocks_attr);
	return -EMSGSIZE;
}

const struct ethnl_request_ops ethnl_intf_caps_request_ops = {
	.request_cmd	= ETHTOOL_MSG_INTF_CAPS_GET,
	.reply_cmd	= ETHTOOL_MSG_INTF_CAPS_GET_REPLY,
	.hdr_attr	= ETHTOOL_A_INTF_CAPS_HEADER,
	.req_info_size	= sizeof(struct intf_caps_req_info),
	.reply_data_size = sizeof(struct intf_caps_reply_data),
	.prepare_data	= intf_caps_prepare_data,
	.reply_size	= intf_caps_reply_size,
	.fill_reply	= intf_caps_fill_reply,
};
