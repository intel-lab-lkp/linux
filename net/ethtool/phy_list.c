// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Bootlin
 *
 * Ethtool netlink operations for Ethernet PHY specific operations
 */
#include "common.h"
#include "netlink.h"

#include <linux/phy.h>
#include <linux/phy_ns.h>

struct phy_list_req_info {
	struct ethnl_req_info		base;
};

#define PHY_MAX_ENTRIES	16

struct phy_list_reply_data {
	struct ethnl_reply_data		base;
	u8 n_phys;
	u32 phy_indices[PHY_MAX_ENTRIES];
};

#define PHY_LIST_REPDATA(__reply_base) \
	container_of(__reply_base, struct phy_list_reply_data, base)

const struct nla_policy ethnl_phy_list_get_policy[ETHTOOL_A_PHY_LIST_HEADER + 1] = {
	[ETHTOOL_A_PHY_LIST_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy_stats),
};

static int phy_list_prepare_data(const struct ethnl_req_info *req_base,
				 struct ethnl_reply_data *reply_base,
				 struct genl_info *info)
{
	struct phy_list_reply_data *data = PHY_LIST_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	struct phy_namespace *phy_ns = &dev->phy_ns;
	struct phy_device *phydev;
	int ret;

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;

	data->n_phys = 0;

	mutex_lock(&phy_ns->ns_lock);
	list_for_each_entry(phydev, &phy_ns->phys, node)
		data->phy_indices[data->n_phys++] = phydev->phyindex;
	mutex_unlock(&phy_ns->ns_lock);

	ethnl_ops_complete(dev);

	return ret;
}

static int phy_list_reply_size(const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct phy_list_reply_data *data = PHY_LIST_REPDATA(reply_base);
	int len = 0;

	len += nla_total_size(sizeof(u8)); /* _PHY_LIST_COUNT */
	len += nla_total_size(data->n_phys * sizeof(u32)); /* Array of _PHY_LIST_INDEX */

	return len;
}

static int phy_list_fill_reply(struct sk_buff *skb,
			       const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct phy_list_reply_data *data = PHY_LIST_REPDATA(reply_base);

	if (nla_put_u8(skb, ETHTOOL_A_PHY_LIST_COUNT, data->n_phys))
		return -EMSGSIZE;

	if (!data->n_phys)
		return 0;

	if (nla_put(skb, ETHTOOL_A_PHY_LIST_INDEX, sizeof(u32) * data->n_phys,
		    data->phy_indices))
		return -EMSGSIZE;

	return 0;
}

const struct ethnl_request_ops ethnl_phy_list_request_ops = {
	.request_cmd		= ETHTOOL_MSG_PHY_LIST_GET,
	.reply_cmd		= ETHTOOL_MSG_PHY_LIST_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_PHY_LIST_HEADER,
	.req_info_size		= sizeof(struct phy_list_req_info),
	.reply_data_size	= sizeof(struct phy_list_reply_data),

	.prepare_data		= phy_list_prepare_data,
	.reply_size		= phy_list_reply_size,
	.fill_reply		= phy_list_fill_reply,
};
