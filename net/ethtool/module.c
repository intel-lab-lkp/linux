// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ethtool.h>

#include "netlink.h"
#include "common.h"
#include "bitset.h"

struct module_req_info {
	struct ethnl_req_info base;
};

struct module_reply_data {
	struct ethnl_reply_data	base;
	struct ethtool_module_power_params power;
};

#define MODULE_REPDATA(__reply_base) \
	container_of(__reply_base, struct module_reply_data, base)

/* MODULE_GET */

const struct nla_policy ethnl_module_get_policy[ETHTOOL_A_MODULE_HEADER + 1] = {
	[ETHTOOL_A_MODULE_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
};

static int module_get_power_cfg(struct net_device *dev,
				struct module_reply_data *data,
				struct netlink_ext_ack *extack)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;

	if (!ops->get_module_power_cfg)
		return 0;

	return ops->get_module_power_cfg(dev, &data->power, extack);
}

static int module_prepare_data(const struct ethnl_req_info *req_base,
			       struct ethnl_reply_data *reply_base,
			       const struct genl_info *info)
{
	struct module_reply_data *data = MODULE_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	int ret;

	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		return ret;

	ret = module_get_power_cfg(dev, data, info->extack);
	if (ret < 0)
		goto out_complete;

out_complete:
	ethnl_ops_complete(dev);
	return ret;
}

static int module_reply_size(const struct ethnl_req_info *req_base,
			     const struct ethnl_reply_data *reply_base)
{
	struct module_reply_data *data = MODULE_REPDATA(reply_base);
	int len = 0;

	if (data->power.policy)
		len += nla_total_size(sizeof(u8));	/* _MODULE_POWER_MODE_POLICY */

	if (data->power.mode)
		len += nla_total_size(sizeof(u8));	/* _MODULE_POWER_MODE */

	if (data->power.min_pwr_allowed)
		len += nla_total_size(sizeof(u32));	/* _MIN_POWER_ALLOWED */

	if (data->power.max_pwr_allowed)
		len += nla_total_size(sizeof(u32));	/* _MAX_POWER_ALLOWED */

	if (data->power.max_pwr_set)
		len += nla_total_size(sizeof(u32));	/* _MAX_POWER_SET */

	return len;
}

static int module_fill_reply(struct sk_buff *skb,
			     const struct ethnl_req_info *req_base,
			     const struct ethnl_reply_data *reply_base)
{
	const struct module_reply_data *data = MODULE_REPDATA(reply_base);
	u32 temp;

	if (data->power.policy &&
	    nla_put_u8(skb, ETHTOOL_A_MODULE_POWER_MODE_POLICY,
		       data->power.policy))
		return -EMSGSIZE;

	if (data->power.mode &&
	    nla_put_u8(skb, ETHTOOL_A_MODULE_POWER_MODE, data->power.mode))
		return -EMSGSIZE;

	temp = data->power.min_pwr_allowed;
	if (temp && nla_put_u32(skb, ETHTOOL_A_MODULE_MIN_POWER_ALLOWED, temp))
		return -EMSGSIZE;

	temp = data->power.max_pwr_allowed;
	if (temp && nla_put_u32(skb, ETHTOOL_A_MODULE_MAX_POWER_ALLOWED, temp))
		return -EMSGSIZE;

	temp = data->power.max_pwr_set;
	if (temp && nla_put_u32(skb, ETHTOOL_A_MODULE_MAX_POWER_SET, temp))
		return -EMSGSIZE;

	return 0;
}

/* MODULE_SET */

const struct nla_policy ethnl_module_set_policy[ETHTOOL_A_MODULE_MAX + 1] = {
	[ETHTOOL_A_MODULE_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_MODULE_POWER_MODE_POLICY] =
		NLA_POLICY_RANGE(NLA_U8, ETHTOOL_MODULE_POWER_MODE_POLICY_HIGH,
				 ETHTOOL_MODULE_POWER_MODE_POLICY_AUTO),
	[ETHTOOL_A_MODULE_MAX_POWER_SET] = { .type = NLA_U32 },
	[ETHTOOL_A_MODULE_MAX_POWER_RESET] = { .type = NLA_U8 },
};

static int
ethnl_set_module_validate(struct ethnl_req_info *req_info,
			  struct genl_info *info)
{
	const struct ethtool_ops *ops = req_info->dev->ethtool_ops;
	struct nlattr **tb = info->attrs;

	if (!tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY] &&
	    !tb[ETHTOOL_A_MODULE_MAX_POWER_SET] &&
	    !tb[ETHTOOL_A_MODULE_MAX_POWER_RESET])
		return 0;

	if (!ops->get_module_power_cfg || !ops->set_module_power_cfg) {
		NL_SET_ERR_MSG(info->extack, "Setting power config is not supported by this device");
		return -EOPNOTSUPP;
	}

	return 1;
}

static void
ethnl_update_policy(enum ethtool_module_power_mode_policy *dst,
		    const struct nlattr *attr, bool *mod)
{
	u8 val = *dst;

	ethnl_update_u8(&val, attr, mod);

	if (mod)
		*dst = val;
}

static int
ethnl_set_module(struct ethnl_req_info *req_info, struct genl_info *info)
{
	struct ethtool_module_power_params power = {};
	struct ethtool_module_power_params power_new;
	struct net_device *dev = req_info->dev;
	struct nlattr **tb = info->attrs;
	const struct ethtool_ops *ops;
	int ret;
	bool mod;

	ops = dev->ethtool_ops;

	ret = ops->get_module_power_cfg(dev, &power, info->extack);
	if (ret < 0)
		return ret;

	power_new.max_pwr_set = power.max_pwr_set;
	power_new.policy = power.policy;

	ethnl_update_u32(&power_new.max_pwr_set,
			 tb[ETHTOOL_A_MODULE_MAX_POWER_SET], &mod);

	if (mod) {
		if (power_new.max_pwr_set > power.max_pwr_allowed) {
			NL_SET_ERR_MSG(info->extack, "Provided value is higher than maximum allowed");
			return -EINVAL;
		} else if (power_new.max_pwr_set < power.min_pwr_allowed) {
			NL_SET_ERR_MSG(info->extack, "Provided value is lower than minimum allowed");
			return -EINVAL;
		}
	}

	ethnl_update_policy(&power_new.policy,
			    tb[ETHTOOL_A_MODULE_POWER_MODE_POLICY], &mod);
	ethnl_update_u8(&power_new.max_pwr_reset,
			tb[ETHTOOL_A_MODULE_MAX_POWER_RESET], &mod);

	if (!mod)
		return 0;

	if (power_new.max_pwr_reset && power_new.max_pwr_set) {
		NL_SET_ERR_MSG(info->extack, "Maximum power set and reset cannot be used at the same time");
		return 0;
	}

	ret = ops->set_module_power_cfg(dev, &power_new, info->extack);
	return ret < 0 ? ret : 1;
}

const struct ethnl_request_ops ethnl_module_request_ops = {
	.request_cmd		= ETHTOOL_MSG_MODULE_GET,
	.reply_cmd		= ETHTOOL_MSG_MODULE_GET_REPLY,
	.hdr_attr		= ETHTOOL_A_MODULE_HEADER,
	.req_info_size		= sizeof(struct module_req_info),
	.reply_data_size	= sizeof(struct module_reply_data),

	.prepare_data		= module_prepare_data,
	.reply_size		= module_reply_size,
	.fill_reply		= module_fill_reply,

	.set_validate		= ethnl_set_module_validate,
	.set			= ethnl_set_module,
	.set_ntf_cmd		= ETHTOOL_MSG_MODULE_NTF,
};
