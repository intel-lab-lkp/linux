// SPDX-License-Identifier: GPL-2.0
/*
 * ulp_ddp.c
 *    Author: Aurelien Aptel <aaptel@nvidia.com>
 *    Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */
#include <net/ulp_ddp.h>
#include "ulp_ddp_gen_nl.h"

#define ULP_DDP_STATS_CNT (sizeof(struct netlink_ulp_ddp_stats) / sizeof(u64))

struct reply_data {
	struct net_device *dev;
	netdevice_tracker tracker;
	void *hdr;
	u32 ifindex;
	DECLARE_BITMAP(hw, ULP_DDP_C_COUNT);
	DECLARE_BITMAP(active, ULP_DDP_C_COUNT);
	struct netlink_ulp_ddp_stats stats;
};

static size_t reply_size(int cmd)
{
	size_t len = 0;

	BUILD_BUG_ON(ULP_DDP_C_COUNT > 64);

	/* ifindex */
	len += nla_total_size(sizeof(u32));

	switch (cmd) {
	case ULP_DDP_CMD_GET:
	case ULP_DDP_CMD_SET:
	case ULP_DDP_CMD_SET_NTF:
		/* hw */
		len += nla_total_size_64bit(sizeof(u64));

		/* active */
		len += nla_total_size_64bit(sizeof(u64));
		break;
	case ULP_DDP_CMD_STATS:
		/* stats */
		len += nla_total_size_64bit(sizeof(u64)) * ULP_DDP_STATS_CNT;
		break;
	}

	return len;
}

/* pre_doit */
int ulp_ddp_get_netdev(const struct genl_split_ops *ops,
		       struct sk_buff *skb, struct genl_info *info)
{
	struct reply_data *data;

	if (GENL_REQ_ATTR_CHECK(info, ULP_DDP_A_DEV_IFINDEX))
		return -EINVAL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->ifindex = nla_get_u32(info->attrs[ULP_DDP_A_DEV_IFINDEX]);
	data->dev = netdev_get_by_index(genl_info_net(info),
					data->ifindex,
					&data->tracker,
					GFP_KERNEL);
	if (!data->dev) {
		kfree(data);
		NL_SET_BAD_ATTR(info->extack,
				info->attrs[ULP_DDP_A_DEV_IFINDEX]);
		return -ENOENT;
	}

	info->user_ptr[0] = data;
	return 0;
}

/* post_doit */
void ulp_ddp_put_netdev(const struct genl_split_ops *ops, struct sk_buff *skb,
			struct genl_info *info)
{
	struct reply_data *data = info->user_ptr[0];

	if (data) {
		if (data->dev)
			netdev_put(data->dev, &data->tracker);
		kfree(data);
	}
}

static int prepare_data(struct reply_data *data, int cmd)
{
	const struct ulp_ddp_dev_ops *ops = data->dev->netdev_ops->ulp_ddp_ops;
	struct ulp_ddp_netdev_caps *caps = &data->dev->ulp_ddp_caps;

	if (!ops)
		return -EOPNOTSUPP;

	switch (cmd) {
	case ULP_DDP_CMD_GET:
	case ULP_DDP_CMD_SET:
	case ULP_DDP_CMD_SET_NTF:
		bitmap_copy(data->hw, caps->hw, ULP_DDP_C_COUNT);
		bitmap_copy(data->active, caps->active, ULP_DDP_C_COUNT);
		break;
	case ULP_DDP_CMD_STATS:
		ops->get_stats(data->dev, &data->stats);
		break;
	}

	return 0;
}

static int fill_data(struct sk_buff *rsp, struct reply_data *data, int cmd,
		     const struct genl_info *info)
{
	u64 *val = (u64 *)&data->stats;
	int attr, i;

	data->hdr = genlmsg_iput(rsp, info);
	if (!data->hdr)
		return -EMSGSIZE;

	switch (cmd) {
	case ULP_DDP_CMD_GET:
	case ULP_DDP_CMD_SET:
	case ULP_DDP_CMD_SET_NTF:
		if (nla_put_u32(rsp, ULP_DDP_A_DEV_IFINDEX, data->ifindex) ||
		    nla_put_u64_64bit(rsp, ULP_DDP_A_DEV_HW,
				      data->hw[0], ULP_DDP_A_DEV_PAD) ||
		    nla_put_u64_64bit(rsp, ULP_DDP_A_DEV_ACTIVE,
				      data->active[0], ULP_DDP_A_DEV_PAD))
			goto err_cancel_msg;
		break;
	case ULP_DDP_CMD_STATS:
		if (nla_put_u32(rsp, ULP_DDP_A_STAT_IFINDEX, data->ifindex))
			goto err_cancel_msg;

		attr = ULP_DDP_A_STAT_PAD + 1;
		for (i = 0; i < ULP_DDP_STATS_CNT; i++, attr++)
			if (nla_put_u64_64bit(rsp, attr, val[i],
					      ULP_DDP_A_STAT_PAD))
				goto err_cancel_msg;
	}
	genlmsg_end(rsp, data->hdr);

	return 0;

err_cancel_msg:
	genlmsg_cancel(rsp, data->hdr);

	return -EMSGSIZE;
}

int ulp_ddp_nl_get_doit(struct sk_buff *req, struct genl_info *info)
{
	struct reply_data *data = info->user_ptr[0];
	struct sk_buff *rsp;
	int ret = 0;

	ret = prepare_data(data, ULP_DDP_CMD_GET);
	if (ret)
		return ret;

	rsp = genlmsg_new(reply_size(ULP_DDP_CMD_GET), GFP_KERNEL);
	if (!rsp)
		return -EMSGSIZE;

	ret = fill_data(rsp, data, ULP_DDP_CMD_GET, info);
	if (ret < 0)
		goto err_rsp;

	return genlmsg_reply(rsp, info);

err_rsp:
	nlmsg_free(rsp);
	return ret;
}

static void ulp_ddp_nl_notify_dev(struct reply_data *data)
{
	struct genl_info info;
	struct sk_buff *ntf;

	if (!genl_has_listeners(&ulp_ddp_nl_family, dev_net(data->dev),
				ULP_DDP_NLGRP_MGMT))
		return;

	genl_info_init_ntf(&info, &ulp_ddp_nl_family, ULP_DDP_CMD_SET_NTF);
	ntf = genlmsg_new(reply_size(ULP_DDP_CMD_GET), GFP_KERNEL);
	if (!ntf)
		return;

	if (fill_data(ntf, data, ULP_DDP_CMD_SET_NTF, &info)) {
		nlmsg_free(ntf);
		return;
	}

	genlmsg_multicast_netns(&ulp_ddp_nl_family, dev_net(data->dev), ntf,
				0, ULP_DDP_NLGRP_MGMT, GFP_KERNEL);
}

static int apply_bits(struct reply_data *data,
		      unsigned long *req_wanted,
		      unsigned long *req_mask,
		      struct netlink_ext_ack *extack)
{
	DECLARE_BITMAP(old_active, ULP_DDP_C_COUNT);
	DECLARE_BITMAP(new_active, ULP_DDP_C_COUNT);
	DECLARE_BITMAP(all_bits, ULP_DDP_C_COUNT);
	DECLARE_BITMAP(tmp, ULP_DDP_C_COUNT);
	const struct ulp_ddp_dev_ops *ops;
	struct ulp_ddp_netdev_caps *caps;
	int ret;

	caps = &data->dev->ulp_ddp_caps;
	ops = data->dev->netdev_ops->ulp_ddp_ops;

	if (!ops)
		return -EOPNOTSUPP;

	/* if (req_mask & ~all_bits) */
	bitmap_fill(all_bits, ULP_DDP_C_COUNT);
	bitmap_andnot(tmp, req_mask, all_bits, ULP_DDP_C_COUNT);
	if (!bitmap_empty(tmp, ULP_DDP_C_COUNT))
		return -EINVAL;

	/* new_active = (old_active & ~req_mask) | (wanted & req_mask)
	 * new_active &= caps_hw
	 */
	bitmap_copy(old_active, caps->active, ULP_DDP_C_COUNT);
	bitmap_and(req_wanted, req_wanted, req_mask, ULP_DDP_C_COUNT);
	bitmap_andnot(new_active, old_active, req_mask, ULP_DDP_C_COUNT);
	bitmap_or(new_active, new_active, req_wanted, ULP_DDP_C_COUNT);
	bitmap_and(new_active, new_active, caps->hw, ULP_DDP_C_COUNT);
	if (!bitmap_equal(old_active, new_active, ULP_DDP_C_COUNT)) {
		ret = ops->set_caps(data->dev, new_active, extack);
		if (ret < 0)
			return ret;
		bitmap_copy(new_active, caps->active, ULP_DDP_C_COUNT);
	}

	/* return 1 to notify */
	return !bitmap_equal(old_active, new_active, ULP_DDP_C_COUNT);
}

int ulp_ddp_nl_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct reply_data *data = info->user_ptr[0];
	unsigned long wanted, wanted_mask;
	struct sk_buff *rsp;
	bool notify;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, ULP_DDP_A_DEV_WANTED) ||
	    GENL_REQ_ATTR_CHECK(info, ULP_DDP_A_DEV_WANTED_MASK))
		return -EINVAL;

	rsp = genlmsg_new(reply_size(ULP_DDP_CMD_STATS), GFP_KERNEL);
	if (!rsp)
		return -EMSGSIZE;

	wanted = nla_get_u64(info->attrs[ULP_DDP_A_DEV_WANTED]);
	wanted_mask = nla_get_u64(info->attrs[ULP_DDP_A_DEV_WANTED_MASK]);

	ret = apply_bits(data, &wanted, &wanted_mask, info->extack);
	if (ret < 0)
		goto err_rsp;

	notify = !!ret;
	ret = prepare_data(data, ULP_DDP_CMD_SET);
	if (ret)
		goto err_rsp;

	ret = fill_data(rsp, data, ULP_DDP_CMD_SET, info);
	if (ret < 0)
		goto err_rsp;

	ret = genlmsg_reply(rsp, info);
	if (notify)
		ulp_ddp_nl_notify_dev(data);

	return ret;

err_rsp:
	nlmsg_free(rsp);

	return ret;
}

int ulp_ddp_nl_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct net_device *netdev;
	struct reply_data data;
	int err = 0;

	rtnl_lock();
	for_each_netdev_dump(net, netdev, cb->args[0]) {
		memset(&data, 0, sizeof(data));
		data.dev = netdev;
		data.ifindex = netdev->ifindex;

		err = prepare_data(&data, ULP_DDP_CMD_GET);
		if (err)
			continue;

		err = fill_data(skb, &data, ULP_DDP_CMD_GET,
				genl_info_dump(cb));
		if (err < 0)
			break;
	}
	rtnl_unlock();

	if (err != -EMSGSIZE)
		return err;

	return skb->len;
}

int ulp_ddp_nl_stats_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct reply_data *data = info->user_ptr[0];
	struct sk_buff *rsp;
	int ret = 0;

	ret = prepare_data(data, ULP_DDP_CMD_STATS);
	if (ret)
		return ret;

	rsp = genlmsg_new(reply_size(ULP_DDP_CMD_STATS), GFP_KERNEL);
	if (!rsp)
		return -EMSGSIZE;

	ret = fill_data(rsp, data, ULP_DDP_CMD_STATS, info);
	if (ret < 0)
		goto err_rsp;

	return genlmsg_reply(rsp, info);

err_rsp:
	nlmsg_free(rsp);
	return ret;
}

int ulp_ddp_nl_stats_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct net_device *netdev;
	struct reply_data data;
	int err = 0;

	rtnl_lock();
	for_each_netdev_dump(net, netdev, cb->args[0]) {
		memset(&data, 0, sizeof(data));
		data.dev = netdev;
		data.ifindex = netdev->ifindex;

		err = prepare_data(&data, ULP_DDP_CMD_STATS);
		if (err)
			continue;

		err = fill_data(skb, &data, ULP_DDP_CMD_STATS,
				genl_info_dump(cb));
		if (err < 0)
			break;
	}
	rtnl_unlock();

	if (err != -EMSGSIZE)
		return err;

	return skb->len;
}

static int __init ulp_ddp_init(void)
{
	int err;

	err = genl_register_family(&ulp_ddp_nl_family);
	if (err)
		return err;

	return 0;
}

subsys_initcall(ulp_ddp_init);
