// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/slab.h>

#include "netlink.h"
#include "common.h"

#define PHY_MSE_CHANNEL_COUNT 4

struct mse_req_info {
	struct ethnl_req_info base;
};

struct mse_snapshot_entry {
	struct phy_mse_snapshot snapshot;
	int channel;
};

struct mse_reply_data {
	struct ethnl_reply_data base;
	struct phy_mse_config config;
	struct mse_snapshot_entry *snapshots;
	unsigned int num_snapshots;
};

static inline struct mse_reply_data *
mse_repdata(const struct ethnl_reply_data *reply_base)
{
	return container_of(reply_base, struct mse_reply_data, base);
}

const struct nla_policy ethnl_mse_get_policy[] = {
	[ETHTOOL_A_MSE_HEADER] = NLA_POLICY_NESTED(ethnl_header_policy_phy),
	[ETHTOOL_A_MSE_CHANNEL] = { .type = NLA_U32 },
};

static int get_snapshot_if_supported(struct phy_device *phydev,
				     struct mse_reply_data *data,
				     unsigned int *idx, u32 cap_bit,
				     int channel_id)
{
	int ret;

	if (data->config.supported_caps & cap_bit) {
		ret = phydev->drv->get_mse_snapshot(phydev, channel_id,
					&data->snapshots[*idx].snapshot);
		if (ret)
			return ret;
		data->snapshots[*idx].channel = channel_id;
		(*idx)++;
	}

	return 0;
}

static int mse_get_channels(struct phy_device *phydev,
			    struct mse_reply_data *data)
{
	unsigned int i = 0;
	int ret;

	if (!data->config.supported_caps)
		return 0;

	data->snapshots = kcalloc(PHY_MSE_CHANNEL_COUNT,
				  sizeof(*data->snapshots), GFP_KERNEL);
	if (!data->snapshots)
		return -ENOMEM;

	/* Priority 1: Individual channels */
	ret = get_snapshot_if_supported(phydev, data, &i, PHY_MSE_CAP_CHANNEL_A,
					PHY_MSE_CHANNEL_A);
	if (ret)
		return ret;
	ret = get_snapshot_if_supported(phydev, data, &i, PHY_MSE_CAP_CHANNEL_B,
					PHY_MSE_CHANNEL_B);
	if (ret)
		return ret;
	ret = get_snapshot_if_supported(phydev, data, &i, PHY_MSE_CAP_CHANNEL_C,
					PHY_MSE_CHANNEL_C);
	if (ret)
		return ret;
	ret = get_snapshot_if_supported(phydev, data, &i, PHY_MSE_CAP_CHANNEL_D,
					PHY_MSE_CHANNEL_D);
	if (ret)
		return ret;

	/* If any individual channels were found, we are done. */
	if (i > 0) {
		data->num_snapshots = i;
		return 0;
	}

	/* Priority 2: Worst channel, if no individual channels supported. */
	ret = get_snapshot_if_supported(phydev, data, &i,
					PHY_MSE_CAP_WORST_CHANNEL,
					PHY_MSE_CHANNEL_WORST);
	if (ret)
		return ret;

	/* If worst channel was found, we are done. */
	if (i > 0) {
		data->num_snapshots = i;
		return 0;
	}

	/* Priority 3: Link-wide, if nothing else is supported. */
	ret = get_snapshot_if_supported(phydev, data, &i, PHY_MSE_CAP_LINK,
					PHY_MSE_CHANNEL_LINK);
	if (ret)
		return ret;

	data->num_snapshots = i;
	return 0;
}

static int mse_get_one_channel(struct phy_device *phydev,
			       struct mse_reply_data *data, int channel)
{
	u32 cap_bit = 0;
	int ret;

	switch (channel) {
	case PHY_MSE_CHANNEL_A:
		cap_bit = PHY_MSE_CAP_CHANNEL_A;
		break;
	case PHY_MSE_CHANNEL_B:
		cap_bit = PHY_MSE_CAP_CHANNEL_B;
		break;
	case PHY_MSE_CHANNEL_C:
		cap_bit = PHY_MSE_CAP_CHANNEL_C;
		break;
	case PHY_MSE_CHANNEL_D:
		cap_bit = PHY_MSE_CAP_CHANNEL_D;
		break;
	case PHY_MSE_CHANNEL_WORST:
		cap_bit = PHY_MSE_CAP_WORST_CHANNEL;
		break;
	case PHY_MSE_CHANNEL_LINK:
		cap_bit = PHY_MSE_CAP_LINK;
		break;
	default:
		return -EINVAL;
	}

	if (!(data->config.supported_caps & cap_bit))
		return -EOPNOTSUPP;

	data->snapshots = kzalloc(sizeof(*data->snapshots), GFP_KERNEL);
	if (!data->snapshots)
		return -ENOMEM;

	ret = phydev->drv->get_mse_snapshot(phydev, channel,
					    &data->snapshots[0].snapshot);
	if (ret)
		return ret;

	data->snapshots[0].channel = channel;
	data->num_snapshots = 1;
	return 0;
}

static int mse_prepare_data(const struct ethnl_req_info *req_base,
			    struct ethnl_reply_data *reply_base,
			    const struct genl_info *info)
{
	struct mse_reply_data *data = mse_repdata(reply_base);
	struct net_device *dev = reply_base->dev;
	struct phy_device *phydev;
	int ret;

	phydev = ethnl_req_get_phydev(req_base, info->attrs,
				      ETHTOOL_A_MSE_HEADER, info->extack);
	if (IS_ERR(phydev))
		return PTR_ERR(phydev);
	if (!phydev)
		return -EOPNOTSUPP;

	ret = ethnl_ops_begin(dev);
	if (ret)
		return ret;

	mutex_lock(&phydev->lock);

	if (!phydev->drv || !phydev->drv->get_mse_config ||
	    !phydev->drv->get_mse_snapshot) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (!phydev->link) {
		ret = -ENETDOWN;
		goto out_unlock;
	}

	ret = phydev->drv->get_mse_config(phydev, &data->config);
	if (ret)
		goto out_unlock;

	if (info->attrs[ETHTOOL_A_MSE_CHANNEL]) {
		u32 channel = nla_get_u32(info->attrs[ETHTOOL_A_MSE_CHANNEL]);

		ret = mse_get_one_channel(phydev, data, channel);
	} else {
		ret = mse_get_channels(phydev, data);
	}

out_unlock:
	mutex_unlock(&phydev->lock);
	ethnl_ops_complete(dev);
	if (ret)
		kfree(data->snapshots);
	return ret;
}

static void mse_cleanup_data(struct ethnl_reply_data *reply_base)
{
	struct mse_reply_data *data = mse_repdata(reply_base);

	kfree(data->snapshots);
}

static int mse_reply_size(const struct ethnl_req_info *req_base,
			  const struct ethnl_reply_data *reply_base)
{
	const struct mse_reply_data *data = mse_repdata(reply_base);
	size_t len = 0;
	unsigned int i;

	/* ETHTOOL_A_MSE_CONFIG */
	len += nla_total_size(0);
	if (data->config.supported_caps & PHY_MSE_CAP_AVG)
		/* ETHTOOL_A_MSE_CONFIG_MAX_AVERAGE_MSE */
		len += nla_total_size(sizeof(u32));
	if (data->config.supported_caps & (PHY_MSE_CAP_PEAK |
					   PHY_MSE_CAP_WORST_PEAK))
		/* ETHTOOL_A_MSE_CONFIG_MAX_PEAK_MSE */
		len += nla_total_size(sizeof(u32));
	/* ETHTOOL_A_MSE_CONFIG_REFRESH_RATE_PS */
	len += nla_total_size(sizeof(u64));
	/* ETHTOOL_A_MSE_CONFIG_NUM_SYMBOLS */
	len += nla_total_size(sizeof(u64));
	/* ETHTOOL_A_MSE_CONFIG_SUPPORTED_CAPS */
	len += nla_total_size(sizeof(u32));

	for (i = 0; i < data->num_snapshots; i++) {
		size_t snapshot_len = 0;

		/* ETHTOOL_A_MSE_SNAPSHOT */
		snapshot_len += nla_total_size(0);
		/* ETHTOOL_A_MSE_SNAPSHOT_CHANNEL */
		snapshot_len += nla_total_size(sizeof(u32));

		if (data->config.supported_caps & PHY_MSE_CAP_AVG)
			snapshot_len += nla_total_size(sizeof(u32));
		if (data->config.supported_caps & PHY_MSE_CAP_PEAK)
			snapshot_len += nla_total_size(sizeof(u32));
		if (data->config.supported_caps & PHY_MSE_CAP_WORST_PEAK)
			snapshot_len += nla_total_size(sizeof(u32));

		len += snapshot_len;
	}

	return len;
}

static int mse_fill_reply(struct sk_buff *skb,
			  const struct ethnl_req_info *req_base,
			  const struct ethnl_reply_data *reply_base)
{
	const struct mse_reply_data *data = mse_repdata(reply_base);
	struct nlattr *config_nest, *snapshot_nest;
	unsigned int i;
	int ret;

	config_nest = nla_nest_start(skb, ETHTOOL_A_MSE_CONFIG);
	if (!config_nest)
		return -EMSGSIZE;

	if (data->config.supported_caps & PHY_MSE_CAP_AVG)
		if (nla_put_u32(skb, ETHTOOL_A_MSE_CONFIG_MAX_AVERAGE_MSE,
				data->config.max_average_mse))
			goto nla_put_config_failure;

	if (data->config.supported_caps & (PHY_MSE_CAP_PEAK |
					   PHY_MSE_CAP_WORST_PEAK))
		if (nla_put_u32(skb, ETHTOOL_A_MSE_CONFIG_MAX_PEAK_MSE,
				data->config.max_peak_mse))
			goto nla_put_config_failure;

	if (nla_put_u64_64bit(skb, ETHTOOL_A_MSE_CONFIG_REFRESH_RATE_PS,
			      data->config.refresh_rate_ps,
			      ETHTOOL_A_MSE_CONFIG_PAD) ||
	    nla_put_u64_64bit(skb, ETHTOOL_A_MSE_CONFIG_NUM_SYMBOLS,
			      data->config.num_symbols,
			      ETHTOOL_A_MSE_CONFIG_PAD) ||
	    nla_put_u32(skb, ETHTOOL_A_MSE_CONFIG_SUPPORTED_CAPS,
			data->config.supported_caps))
		goto nla_put_config_failure;

	nla_nest_end(skb, config_nest);

	for (i = 0; i < data->num_snapshots; i++) {
		const struct mse_snapshot_entry *s = &data->snapshots[i];

		snapshot_nest = nla_nest_start(skb, ETHTOOL_A_MSE_SNAPSHOT);
		if (!snapshot_nest)
			return -EMSGSIZE;

		ret = nla_put_u32(skb, ETHTOOL_A_MSE_SNAPSHOT_CHANNEL,
				  s->channel);
		if (ret)
			goto nla_put_failure;

		if (data->config.supported_caps & PHY_MSE_CAP_AVG) {
			ret = nla_put_u32(skb,
					  ETHTOOL_A_MSE_SNAPSHOT_AVERAGE_MSE,
					  s->snapshot.average_mse);
			if (ret)
				goto nla_put_failure;
		}
		if (data->config.supported_caps & PHY_MSE_CAP_PEAK) {
			ret = nla_put_u32(skb, ETHTOOL_A_MSE_SNAPSHOT_PEAK_MSE,
					  s->snapshot.peak_mse);
			if (ret)
				goto nla_put_failure;
		}
		if (data->config.supported_caps & PHY_MSE_CAP_WORST_PEAK) {
			ret = nla_put_u32(skb,
					  ETHTOOL_A_MSE_SNAPSHOT_WORST_PEAK_MSE,
					  s->snapshot.worst_peak_mse);
			if (ret)
				goto nla_put_failure;
		}

		nla_nest_end(skb, snapshot_nest);
	}

	return 0;

nla_put_config_failure:
	nla_nest_cancel(skb, config_nest);
	return -EMSGSIZE;

nla_put_failure:
	nla_nest_cancel(skb, snapshot_nest);
	return -EMSGSIZE;
}

const struct ethnl_request_ops ethnl_mse_request_ops = {
	.request_cmd = ETHTOOL_MSG_MSE_GET,
	.reply_cmd = ETHTOOL_MSG_MSE_GET_REPLY,
	.hdr_attr = ETHTOOL_A_MSE_HEADER,
	.req_info_size = sizeof(struct mse_req_info),
	.reply_data_size = sizeof(struct mse_reply_data),

	.prepare_data = mse_prepare_data,
	.cleanup_data = mse_cleanup_data,
	.reply_size = mse_reply_size,
	.fill_reply = mse_fill_reply,
};
