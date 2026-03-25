// SPDX-License-Identifier: GPL-2.0-only

#include "netlink.h"
#include "common.h"

struct loopback_req_info {
	struct ethnl_req_info base;
	enum ethtool_loopback_component component;
	u32 id;
	char name[ETH_GSTRING_LEN];
	bool lookup_by_name;
	u32 index;
};

#define LOOPBACK_REQINFO(__req_base) \
	container_of(__req_base, struct loopback_req_info, base)

struct loopback_reply_data {
	struct ethnl_reply_data base;
	struct ethtool_loopback_entry entry;
};

#define LOOPBACK_REPDATA(__reply_base) \
	container_of(__reply_base, struct loopback_reply_data, base)

/* GET */

static const struct nla_policy
ethnl_loopback_entry_policy[ETHTOOL_A_LOOPBACK_ENTRY_MAX + 1] = {
	[ETHTOOL_A_LOOPBACK_ENTRY_COMPONENT] =
		NLA_POLICY_MAX(NLA_U32, ETHTOOL_LOOPBACK_COMPONENT_MODULE),
	[ETHTOOL_A_LOOPBACK_ENTRY_ID] = NLA_POLICY_MIN(NLA_U32, 1),
	[ETHTOOL_A_LOOPBACK_ENTRY_NAME] = { .type = NLA_NUL_STRING,
					    .len = ETH_GSTRING_LEN },
	[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION] =
		NLA_POLICY_MASK(NLA_U8, ETHTOOL_LOOPBACK_DIRECTION_LOCAL |
				ETHTOOL_LOOPBACK_DIRECTION_REMOTE),
	[ETHTOOL_A_LOOPBACK_ENTRY_DEPTH] = { .type = NLA_U8 },
};

const struct nla_policy
ethnl_loopback_get_policy[ETHTOOL_A_LOOPBACK_MAX + 1] = {
	[ETHTOOL_A_LOOPBACK_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_LOOPBACK_ENTRY] =
		NLA_POLICY_NESTED(ethnl_loopback_entry_policy),
};

static int loopback_parse_request(struct ethnl_req_info *req_base,
				  const struct genl_info *info,
				  struct nlattr **tb,
				  struct netlink_ext_ack *extack)
{
	struct loopback_req_info *req_info = LOOPBACK_REQINFO(req_base);
	struct nlattr *entry_tb[ETHTOOL_A_LOOPBACK_ENTRY_MAX + 1];
	int ret;

	if (!tb[ETHTOOL_A_LOOPBACK_ENTRY])
		return 0;

	ret = nla_parse_nested(entry_tb, ETHTOOL_A_LOOPBACK_ENTRY_MAX,
			       tb[ETHTOOL_A_LOOPBACK_ENTRY],
			       ethnl_loopback_entry_policy, extack);
	if (ret < 0)
		return ret;

	if (!entry_tb[ETHTOOL_A_LOOPBACK_ENTRY_COMPONENT] ||
	    !entry_tb[ETHTOOL_A_LOOPBACK_ENTRY_NAME]) {
		NL_SET_ERR_MSG(extack,
			       "component and name required for loopback lookup");
		return -EINVAL;
	}

	req_info->component =
		nla_get_u32(entry_tb[ETHTOOL_A_LOOPBACK_ENTRY_COMPONENT]);
	if (entry_tb[ETHTOOL_A_LOOPBACK_ENTRY_ID])
		req_info->id =
			nla_get_u32(entry_tb[ETHTOOL_A_LOOPBACK_ENTRY_ID]);
	nla_strscpy(req_info->name, entry_tb[ETHTOOL_A_LOOPBACK_ENTRY_NAME],
		    sizeof(req_info->name));
	req_info->lookup_by_name = true;

	return 0;
}

static int loopback_get(struct net_device *dev,
			enum ethtool_loopback_component component, u32 id,
			const char *name,
			struct ethtool_loopback_entry *entry)
{
	switch (component) {
	case ETHTOOL_LOOPBACK_COMPONENT_MODULE:
		return ethtool_cmis_get_loopback(dev, name, entry);
	default:
		return -EOPNOTSUPP;
	}
}

static int loopback_get_by_index(struct net_device *dev, u32 index,
				 struct ethtool_loopback_entry *entry)
{
	return ethtool_cmis_get_loopback_by_index(dev, index, entry);
}

static int loopback_prepare_data(const struct ethnl_req_info *req_base,
				 struct ethnl_reply_data *reply_base,
				 const struct genl_info *info)
{
	const struct loopback_req_info *req_info = LOOPBACK_REQINFO(req_base);
	struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	int ret;

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;

	if (req_info->lookup_by_name)
		ret = loopback_get(dev, req_info->component, req_info->id,
				   req_info->name, &data->entry);
	else
		ret = loopback_get_by_index(dev, req_info->index, &data->entry);

	ethnl_ops_complete(dev);

	return ret;
}

static int loopback_reply_size(const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	return nla_total_size(0) +			/* nest */
	       nla_total_size(sizeof(u32)) +		/* component */
	       nla_total_size(sizeof(u32)) +		/* id */
	       nla_total_size(sizeof(u8)) +		/* supported */
	       nla_total_size(sizeof(u8)) +		/* direction */
	       nla_total_size(sizeof(u8)) +		/* depth */
	       nla_total_size(ETH_GSTRING_LEN);		/* name */
}

static int loopback_fill_reply(struct sk_buff *skb,
			       const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct loopback_reply_data *data = LOOPBACK_REPDATA(reply_base);
	const struct ethtool_loopback_entry *entry = &data->entry;
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

	if (entry->depth &&
	    nla_put_u8(skb, ETHTOOL_A_LOOPBACK_ENTRY_DEPTH, entry->depth))
		goto err_cancel;

	if (nla_put_u8(skb, ETHTOOL_A_LOOPBACK_ENTRY_SUPPORTED,
		       entry->supported) ||
	    nla_put_u8(skb, ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION,
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

/* SET */

const struct nla_policy
ethnl_loopback_set_policy[ETHTOOL_A_LOOPBACK_MAX + 1] = {
	[ETHTOOL_A_LOOPBACK_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_LOOPBACK_ENTRY] =
		NLA_POLICY_NESTED(ethnl_loopback_entry_policy),
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

	if (tb[ETHTOOL_A_LOOPBACK_ENTRY_DEPTH])
		entry->depth = nla_get_u8(tb[ETHTOOL_A_LOOPBACK_ENTRY_DEPTH]);

	if (!tb[ETHTOOL_A_LOOPBACK_ENTRY_NAME]) {
		NL_SET_ERR_MSG_ATTR(extack, attr, "loopback name is required");
		return -EINVAL;
	}
	nla_strscpy(entry->name, tb[ETHTOOL_A_LOOPBACK_ENTRY_NAME],
		    sizeof(entry->name));

	if (!tb[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION]) {
		NL_SET_ERR_MSG_ATTR(extack, attr,
				    "loopback direction is required");
		return -EINVAL;
	}

	entry->direction = nla_get_u8(tb[ETHTOOL_A_LOOPBACK_ENTRY_DIRECTION]);

	return 0;
}

static int __loopback_set(struct net_device *dev,
			  const struct ethtool_loopback_entry *entry,
			  struct netlink_ext_ack *extack)
{
	switch (entry->component) {
	case ETHTOOL_LOOPBACK_COMPONENT_MODULE:
		return ethtool_cmis_set_loopback(dev, entry, extack);
	default:
		return -EOPNOTSUPP;
	}
}

static int loopback_set(struct ethnl_req_info *req_info,
			struct genl_info *info)
{
	struct net_device *dev = req_info->dev;
	struct ethtool_loopback_entry entry;
	int rem, ret, mod = 0;
	struct nlattr *attr;
	bool found = false;

	nla_for_each_attr(attr, genlmsg_data(info->genlhdr),
			  genlmsg_len(info->genlhdr), rem) {
		if (nla_type(attr) != ETHTOOL_A_LOOPBACK_ENTRY)
			continue;

		found = true;
		memset(&entry, 0, sizeof(entry));
		ret = loopback_parse_entry(attr, &entry, info->extack);
		if (ret < 0)
			return ret;

		ret = __loopback_set(dev, &entry, info->extack);
		if (ret < 0)
			return ret;
		if (ret > 0)
			mod = 1;
	}

	if (!found) {
		NL_SET_ERR_MSG(info->extack, "no loopback entries specified");
		return -EINVAL;
	}

	return mod;
}

static int loopback_dump_one_dev(struct sk_buff *skb,
				 struct ethnl_dump_ctx *ctx,
				 unsigned long *pos_sub,
				 const struct genl_info *info)
{
	struct loopback_req_info *req_info =
		container_of(ctx->req_info, struct loopback_req_info, base);
	int ret;

	for (;; (*pos_sub)++) {
		req_info->index = *pos_sub;
		ret = ethnl_default_dump_one(skb, ctx->req_info->dev, ctx,
					     info);
		if (ret == -EOPNOTSUPP)
			break;
		if (ret)
			return ret;
	}

	*pos_sub = 0;

	return 0;
}

const struct ethnl_request_ops ethnl_loopback_request_ops = {
	.request_cmd		= ETHTOOL_MSG_LOOPBACK_GET,
	.reply_cmd		= ETHTOOL_MSG_LOOPBACK_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_LOOPBACK_HEADER,
	.req_info_size		= sizeof(struct loopback_req_info),
	.reply_data_size	= sizeof(struct loopback_reply_data),

	.parse_request		= loopback_parse_request,
	.prepare_data		= loopback_prepare_data,
	.reply_size		= loopback_reply_size,
	.fill_reply		= loopback_fill_reply,
	.dump_one_dev		= loopback_dump_one_dev,

	.set			= loopback_set,
	.set_ntf_cmd		= ETHTOOL_MSG_LOOPBACK_NTF,
};
