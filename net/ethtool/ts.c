// SPDX-License-Identifier: GPL-2.0-only

#include <linux/net_tstamp.h>
#include <linux/phy.h>

#include "netlink.h"
#include "common.h"
#include "bitset.h"

struct ts_req_info {
	struct ethnl_req_info		base;
};

struct ts_reply_data {
	struct ethnl_reply_data		base;
	u32				ts_layer;
};

#define TS_REPDATA(__reply_base) \
	container_of(__reply_base, struct ts_reply_data, base)

/* TS_GET */
const struct nla_policy ethnl_ts_get_policy[] = {
	[ETHTOOL_A_TS_HEADER]		=
		NLA_POLICY_NESTED(ethnl_header_policy),
};

static int ts_prepare_data(const struct ethnl_req_info *req_base,
			   struct ethnl_reply_data *reply_base,
			   const struct genl_info *info)
{
	struct ts_reply_data *data = TS_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	const struct ethtool_ops *ops = dev->ethtool_ops;
	int ret;

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;

	if (phy_has_tsinfo(dev->phydev))
		data->ts_layer = PHYLIB_TIMESTAMPING;
	else if (ops->get_ts_info)
		data->ts_layer = NETDEV_TIMESTAMPING;
	else
		data->ts_layer = NO_TIMESTAMPING;

	ethnl_ops_complete(dev);

	return ret;
}

static int ts_reply_size(const struct ethnl_req_info *req_base,
			 const struct ethnl_reply_data *reply_base)
{
	return nla_total_size(sizeof(u32));
}

static int ts_fill_reply(struct sk_buff *skb,
			 const struct ethnl_req_info *req_base,
			 const struct ethnl_reply_data *reply_base)
{
	struct ts_reply_data *data = TS_REPDATA(reply_base);

	return nla_put_u32(skb, ETHTOOL_A_TS_LAYER, data->ts_layer);
}

const struct ethnl_request_ops ethnl_ts_request_ops = {
	.request_cmd		= ETHTOOL_MSG_TS_GET,
	.reply_cmd		= ETHTOOL_MSG_TS_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_TS_HEADER,
	.req_info_size		= sizeof(struct ts_req_info),
	.reply_data_size	= sizeof(struct ts_reply_data),

	.prepare_data		= ts_prepare_data,
	.reply_size		= ts_reply_size,
	.fill_reply		= ts_fill_reply,
};
