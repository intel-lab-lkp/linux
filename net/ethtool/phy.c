// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Bootlin
 *
 */
#include "common.h"
#include "netlink.h"

#include <linux/phy.h>
#include <linux/phy_ns.h>

struct phy_req_info {
	struct ethnl_req_info		base;
	u32				phyindex;
};

struct phy_reply_data {
	struct ethnl_reply_data		base;
	u32				phyindex;
	const char			*drvname;
	enum phy_upstream_type		upstream;
	u32				id;
};

#define PHY_REQINFO(__req_base) \
	container_of(__req_base, struct phy_req_info, base)
#define PHY_REPDATA(__reply_base) \
	container_of(__reply_base, struct phy_reply_data, base)

const struct nla_policy ethnl_phy_get_policy[ETHTOOL_A_PHY_INDEX + 1] = {
	[ETHTOOL_A_PHY_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_PHY_INDEX] = NLA_POLICY_MAX(NLA_U32, 255),
};

static int phy_parse_request(struct ethnl_req_info *req_base,
			     struct nlattr **tb,
			     struct netlink_ext_ack *extack)
{
	struct phy_req_info *req_info = PHY_REQINFO(req_base);

	if (!tb[ETHTOOL_A_PHY_INDEX])
		return -EINVAL;

	req_info->phyindex = nla_get_u32(tb[ETHTOOL_A_PHY_INDEX]);

	return 0;
}

static int phy_prepare_data(const struct ethnl_req_info *req_base,
			    struct ethnl_reply_data *reply_base,
			    struct genl_info *info)
{
	struct phy_req_info *req_info = PHY_REQINFO(req_base);
	struct phy_reply_data *data = PHY_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	struct phy_namespace *phy_ns = &dev->phy_ns;
	struct phy_device *phydev;
	int ret;

	phydev = phy_ns_get_by_index(phy_ns, req_info->phyindex);
	if (!phydev)
		return -ENODEV;

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;

	data->phyindex = req_info->phyindex;
	data->drvname = phydev->drv->name;
	if (phydev->is_on_sfp_module)
		data->upstream = PHY_UPSTREAM_SFP;
	else if (phydev->attached_dev)
		data->upstream = PHY_UPSTREAM_MAC;
	else
		data->upstream = PHY_UPSTREAM_PHY;

	data->id = phydev->phy_id;

	ethnl_ops_complete(dev);

	return ret;
}

static int phy_reply_size(const struct ethnl_req_info *req_base,
			  const struct ethnl_reply_data *reply_base)
{
	const struct phy_reply_data *data = PHY_REPDATA(reply_base);
	int len = 0;

	len += nla_total_size(sizeof(u32));	/* ETHTOOL_A_PHY_INDEX */
	len += ethnl_strz_size(data->drvname);	/* ETHTOOL_A_DRVNAME */
	len += nla_total_size(sizeof(u8));	/* ETHTOOL_A_PHY_UPSTREAM_TYPE */
	len += nla_total_size(sizeof(u32));	/* ETHTOOL_A_PHY_ID */

	return len;
}

static int phy_fill_reply(struct sk_buff *skb,
			  const struct ethnl_req_info *req_base,
			  const struct ethnl_reply_data *reply_base)
{
	const struct phy_reply_data *data = PHY_REPDATA(reply_base);

	if (nla_put_u32(skb, ETHTOOL_A_PHY_INDEX, data->phyindex) ||
	    ethnl_put_strz(skb, ETHTOOL_A_PHY_DRVNAME, data->drvname) ||
	    nla_put_u8(skb, ETHTOOL_A_PHY_UPSTREAM_TYPE, data->upstream) ||
	    nla_put_u32(skb, ETHTOOL_A_PHY_ID, data->id))
		return -EMSGSIZE;

	return 0;
}

const struct ethnl_request_ops ethnl_phy_request_ops = {
	.request_cmd		= ETHTOOL_MSG_PHY_GET,
	.reply_cmd		= ETHTOOL_MSG_PHY_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_PHY_HEADER,
	.req_info_size		= sizeof(struct phy_req_info),
	.reply_data_size	= sizeof(struct phy_reply_data),

	.parse_request		= phy_parse_request,
	.prepare_data		= phy_prepare_data,
	.reply_size		= phy_reply_size,
	.fill_reply		= phy_fill_reply,
};
