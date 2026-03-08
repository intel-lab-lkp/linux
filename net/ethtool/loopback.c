// SPDX-License-Identifier: GPL-2.0-only

#include "netlink.h"
#include "common.h"

struct loopback_req_info {
	struct ethnl_req_info base;
};

struct loopback_reply_data {
	struct ethnl_reply_data base;
	struct ethtool_loopback_cfg cfg;
};

#define LOOPBACK_REPDATA(__reply_base) \
	container_of(__reply_base, struct loopback_reply_data, base)

/* GET */

static const struct nla_policy
ethnl_loopback_entry_policy[ETHTOOL_A_LOOPBACK_ENTRY_MAX + 1] = {
	[ETHTOOL_A_LOOPBACK_ENTRY_COMPONENT] =
		NLA_POLICY_MAX(NLA_U32, ETHTOOL_LOOPBACK_COMPONENT_MODULE),
	[ETHTOOL_A_LOOPBACK_ENTRY_ID] =
		NLA_POLICY_MIN(NLA_U32, 1),
	[ETHTOOL_A_LOOPBACK_ENTRY_NAME] =
		{ .type = NLA_NUL_STRING, .len = ETH_GSTRING_LEN - 1 },
	[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION] =
		NLA_POLICY_MASK(NLA_U32, ETHTOOL_LOOPBACK_DIRECTION_NEAR_END |
				ETHTOOL_LOOPBACK_DIRECTION_FAR_END),
};

const struct nla_policy ethnl_loopback_get_policy[] = {
	[ETHTOOL_A_LOOPBACK_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
};

static int loopback_get_entries(struct net_device *dev,
				struct ethtool_loopback_cfg *cfg)
{
	return ethtool_cmis_get_loopback(dev, cfg);
}

static int loopback_prepare_data(const struct ethnl_req_info *req_base,
				 struct ethnl_reply_data *reply_base,
				 const struct genl_info *info)
{
	struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	int ret;

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;

	ret = loopback_get_entries(dev, &data->cfg);

	ethnl_ops_complete(dev);

	return ret;
}

static int loopback_reply_size(const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	int entry_size;

	/* Per-entry: nest + component + id + name + supported + direction */
	entry_size = nla_total_size(0) +		/* nest */
		nla_total_size(sizeof(u32)) +		/* component */
		nla_total_size(sizeof(u32)) +		/* id */
		nla_total_size(sizeof(u32)) +		/* supported */
		nla_total_size(sizeof(u32)) +		/* direction */
		nla_total_size(ETH_GSTRING_LEN);	/* name */

	return data->cfg.n_entries * entry_size;
}

static int loopback_fill_entry(struct sk_buff *skb,
			       const struct ethtool_loopback_entry *entry)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, ETHTOOL_A_LOOPBACK_ENTRY);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u32(skb, ETHTOOL_A_LOOPBACK_ENTRY_COMPONENT,
			entry->component))
		goto err_cancel;

	if (entry->id &&
	    nla_put_u32(skb, ETHTOOL_A_LOOPBACK_ENTRY_ID, entry->id))
		goto err_cancel;

	if (nla_put_u32(skb, ETHTOOL_A_LOOPBACK_ENTRY_SUPPORTED,
			entry->supported) ||
	    nla_put_u32(skb, ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION,
			entry->direction) ||
	    nla_put_string(skb, ETHTOOL_A_LOOPBACK_ENTRY_NAME,
			   entry->name))
		goto err_cancel;

	nla_nest_end(skb, nest);
	return 0;

err_cancel:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int loopback_fill_reply(struct sk_buff *skb,
			       const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	const struct ethtool_loopback_cfg *cfg = &data->cfg;
	u32 i;

	for (i = 0; i < cfg->n_entries; i++) {
		int ret = loopback_fill_entry(skb, &cfg->entries[i]);

		if (ret < 0)
			return ret;
	}

	return 0;
}

/* SET */

const struct nla_policy ethnl_loopback_set_policy[ETHTOOL_A_LOOPBACK_ENTRY + 1] = {
	[ETHTOOL_A_LOOPBACK_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_LOOPBACK_ENTRY]  = NLA_POLICY_NESTED(ethnl_loopback_entry_policy),
};

static int loopback_parse_entry(struct nlattr *attr,
				struct ethtool_loopback_entry *entry,
				struct netlink_ext_ack *extack)
{
	struct nlattr *tb[ETHTOOL_A_LOOPBACK_ENTRY_MAX + 1];
	int ret;

	ret = nla_parse_nested(tb, ETHTOOL_A_LOOPBACK_ENTRY_MAX, attr,
			       ethnl_loopback_entry_policy, extack);
	if (ret < 0)
		return ret;

	if (!tb[ETHTOOL_A_LOOPBACK_ENTRY_COMPONENT]) {
		NL_SET_ERR_MSG_ATTR(extack, attr,
				    "loopback component is required");
		return -EINVAL;
	}

	entry->component = nla_get_u32(tb[ETHTOOL_A_LOOPBACK_ENTRY_COMPONENT]);

	if (tb[ETHTOOL_A_LOOPBACK_ENTRY_ID])
		entry->id = nla_get_u32(tb[ETHTOOL_A_LOOPBACK_ENTRY_ID]);

	if (!tb[ETHTOOL_A_LOOPBACK_ENTRY_NAME]) {
		NL_SET_ERR_MSG_ATTR(extack, attr,
				    "loopback name is required");
		return -EINVAL;
	}
	nla_strscpy(entry->name, tb[ETHTOOL_A_LOOPBACK_ENTRY_NAME],
		    sizeof(entry->name));

	if (!tb[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION]) {
		NL_SET_ERR_MSG_ATTR(extack, attr,
				    "loopback direction is required");
		return -EINVAL;
	}

	entry->direction = nla_get_u32(tb[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION]);

	return 0;
}

static int loopback_set_one(struct net_device *dev,
			    const struct ethtool_loopback_entry *entry,
			    struct netlink_ext_ack *extack)
{
	switch (entry->component) {
	case ETHTOOL_LOOPBACK_COMPONENT_MODULE:
		return ethtool_cmis_set_loopback_one(dev, entry, extack);
	default:
		return -EOPNOTSUPP;
	}
}

static int ethnl_set_loopback(struct ethnl_req_info *req_info,
			      struct genl_info *info)
{
	struct net_device *dev = req_info->dev;
	struct ethtool_loopback_cfg cfg = {};
	int rem, ret, mod = 0;
	struct nlattr *attr;
	u32 i;

	nla_for_each_attr(attr, genlmsg_data(info->genlhdr),
			  genlmsg_len(info->genlhdr), rem) {
		if (nla_type(attr) != ETHTOOL_A_LOOPBACK_ENTRY)
			continue;

		if (cfg.n_entries >= ETHTOOL_LOOPBACK_MAX_ENTRIES) {
			NL_SET_ERR_MSG(info->extack,
				       "too many loopback entries");
			return -EINVAL;
		}

		ret = loopback_parse_entry(attr, &cfg.entries[cfg.n_entries],
					   info->extack);
		if (ret < 0)
			return ret;

		cfg.n_entries++;
	}

	if (!cfg.n_entries) {
		NL_SET_ERR_MSG(info->extack, "no loopback entries specified");
		return -EINVAL;
	}

	for (i = 0; i < cfg.n_entries; i++) {
		ret = loopback_set_one(dev, &cfg.entries[i], info->extack);
		if (ret < 0)
			return ret;
		if (ret > 0)
			mod = 1;
	}

	return mod;
}

const struct ethnl_request_ops ethnl_loopback_request_ops = {
	.request_cmd		= ETHTOOL_MSG_LOOPBACK_GET,
	.reply_cmd		= ETHTOOL_MSG_LOOPBACK_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_LOOPBACK_HEADER,
	.req_info_size		= sizeof(struct loopback_req_info),
	.reply_data_size	= sizeof(struct loopback_reply_data),

	.prepare_data		= loopback_prepare_data,
	.reply_size		= loopback_reply_size,
	.fill_reply		= loopback_fill_reply,

	.set			= ethnl_set_loopback,
	.set_ntf_cmd		= ETHTOOL_MSG_LOOPBACK_NTF,
};
