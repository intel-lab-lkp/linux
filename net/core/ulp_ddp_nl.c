// SPDX-License-Identifier: GPL-2.0
/*
 * ulp_ddp_nl.c
 *    Author: Aurelien Aptel <aaptel@nvidia.com>
 *    Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */
#include <net/ulp_ddp.h>
#include "ulp_ddp_gen_nl.h"

#define ULP_DDP_STATS_CNT (sizeof(struct ulp_ddp_stats) / sizeof(u64))

struct ulp_ddp_reply_context {
	struct net_device *dev;
	netdevice_tracker tracker;
	void *hdr;
	u32 ifindex;
	struct ulp_ddp_dev_caps caps;
	struct ulp_ddp_stats stats;
};

static size_t ulp_ddp_reply_size(int cmd)
{
	size_t len = 0;

	BUILD_BUG_ON(ULP_DDP_CAP_COUNT > 64);

	/* ifindex */
	len += nla_total_size(sizeof(u32));

	switch (cmd) {
	case ULP_DDP_CMD_CAPS_GET:
	case ULP_DDP_CMD_CAPS_SET:
	case ULP_DDP_CMD_CAPS_SET_NTF:
		/* hw */
		len += nla_total_size_64bit(sizeof(u64));

		/* active */
		len += nla_total_size_64bit(sizeof(u64));
		break;
	case ULP_DDP_CMD_STATS_GET:
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
	struct ulp_ddp_reply_context *ctx;

	if (GENL_REQ_ATTR_CHECK(info, ULP_DDP_A_CAPS_IFINDEX))
		return -EINVAL;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->ifindex = nla_get_u32(info->attrs[ULP_DDP_A_CAPS_IFINDEX]);
	ctx->dev = netdev_get_by_index(genl_info_net(info),
				       ctx->ifindex,
				       &ctx->tracker,
				       GFP_KERNEL);
	if (!ctx->dev) {
		kfree(ctx);
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[ULP_DDP_A_CAPS_IFINDEX],
				    "Network interface does not exist");
		return -ENODEV;
	}

	if (!ctx->dev->netdev_ops->ulp_ddp_ops) {
		netdev_put(ctx->dev, &ctx->tracker);
		kfree(ctx);
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[ULP_DDP_A_CAPS_IFINDEX],
				    "Network interface does not support ULP DDP");
		return -EOPNOTSUPP;
	}

	info->user_ptr[0] = ctx;
	return 0;
}

/* post_doit */
void ulp_ddp_put_netdev(const struct genl_split_ops *ops, struct sk_buff *skb,
			struct genl_info *info)
{
	struct ulp_ddp_reply_context *ctx = info->user_ptr[0];

	netdev_put(ctx->dev, &ctx->tracker);
	kfree(ctx);
}

static int ulp_ddp_prepare_context(struct ulp_ddp_reply_context *ctx, int cmd)
{
	const struct ulp_ddp_dev_ops *ops = ctx->dev->netdev_ops->ulp_ddp_ops;

	switch (cmd) {
	case ULP_DDP_CMD_CAPS_GET:
	case ULP_DDP_CMD_CAPS_SET:
	case ULP_DDP_CMD_CAPS_SET_NTF:
		ops->get_caps(ctx->dev, &ctx->caps);
		break;
	case ULP_DDP_CMD_STATS_GET:
		ops->get_stats(ctx->dev, &ctx->stats);
		break;
	}

	return 0;
}

static int ulp_ddp_write_reply(struct sk_buff *rsp,
			       struct ulp_ddp_reply_context *ctx,
			       int cmd,
			       const struct genl_info *info)
{
	ctx->hdr = genlmsg_iput(rsp, info);
	if (!ctx->hdr)
		return -EMSGSIZE;

	switch (cmd) {
	case ULP_DDP_CMD_CAPS_GET:
	case ULP_DDP_CMD_CAPS_SET:
	case ULP_DDP_CMD_CAPS_SET_NTF:
		if (nla_put_u32(rsp, ULP_DDP_A_CAPS_IFINDEX, ctx->ifindex) ||
		    nla_put_uint(rsp, ULP_DDP_A_CAPS_HW, ctx->caps.hw[0]) ||
		    nla_put_uint(rsp, ULP_DDP_A_CAPS_ACTIVE,
				 ctx->caps.active[0]))
			goto err_cancel_msg;
		break;
	case ULP_DDP_CMD_STATS_GET:
		if (nla_put_u32(rsp, ULP_DDP_A_STATS_IFINDEX, ctx->ifindex) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_SK_ADD,
				 ctx->stats.rx_nvmeotcp_sk_add) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_SK_ADD_FAIL,
				 ctx->stats.rx_nvmeotcp_sk_add_fail) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_SK_DEL,
				 ctx->stats.rx_nvmeotcp_sk_del) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_SETUP,
				 ctx->stats.rx_nvmeotcp_ddp_setup) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_SETUP_FAIL,
				 ctx->stats.rx_nvmeotcp_ddp_setup_fail) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_TEARDOWN,
				 ctx->stats.rx_nvmeotcp_ddp_teardown) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_DROP,
				 ctx->stats.rx_nvmeotcp_drop) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_RESYNC,
				 ctx->stats.rx_nvmeotcp_resync) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_PACKETS,
				 ctx->stats.rx_nvmeotcp_packets) ||
		    nla_put_uint(rsp,
				 ULP_DDP_A_STATS_RX_NVME_TCP_BYTES,
				 ctx->stats.rx_nvmeotcp_bytes))
			goto err_cancel_msg;
	}
	genlmsg_end(rsp, ctx->hdr);

	return 0;

err_cancel_msg:
	genlmsg_cancel(rsp, ctx->hdr);

	return -EMSGSIZE;
}

int ulp_ddp_nl_caps_get_doit(struct sk_buff *req, struct genl_info *info)
{
	struct ulp_ddp_reply_context *ctx = info->user_ptr[0];
	struct sk_buff *rsp;
	int ret = 0;

	ret = ulp_ddp_prepare_context(ctx, ULP_DDP_CMD_CAPS_GET);
	if (ret)
		return ret;

	rsp = genlmsg_new(ulp_ddp_reply_size(ULP_DDP_CMD_CAPS_GET), GFP_KERNEL);
	if (!rsp)
		return -EMSGSIZE;

	ret = ulp_ddp_write_reply(rsp, ctx, ULP_DDP_CMD_CAPS_GET, info);
	if (ret < 0)
		goto err_rsp;

	return genlmsg_reply(rsp, info);

err_rsp:
	nlmsg_free(rsp);
	return ret;
}

static void ulp_ddp_nl_notify_dev(struct ulp_ddp_reply_context *ctx)
{
	struct genl_info info;
	struct sk_buff *ntf;

	if (!genl_has_listeners(&ulp_ddp_nl_family, dev_net(ctx->dev),
				ULP_DDP_NLGRP_MGMT))
		return;

	genl_info_init_ntf(&info, &ulp_ddp_nl_family, ULP_DDP_CMD_CAPS_SET_NTF);
	ntf = genlmsg_new(ulp_ddp_reply_size(ULP_DDP_CMD_CAPS_GET), GFP_KERNEL);
	if (!ntf)
		return;

	if (ulp_ddp_write_reply(ntf, ctx, ULP_DDP_CMD_CAPS_SET_NTF, &info)) {
		nlmsg_free(ntf);
		return;
	}

	genlmsg_multicast_netns(&ulp_ddp_nl_family, dev_net(ctx->dev), ntf,
				0, ULP_DDP_NLGRP_MGMT, GFP_KERNEL);
}

static int ulp_ddp_apply_bits(struct ulp_ddp_reply_context *ctx,
			      unsigned long *req_wanted,
			      unsigned long *req_mask,
			      struct genl_info *info)
{
	DECLARE_BITMAP(old_active, ULP_DDP_CAP_COUNT);
	DECLARE_BITMAP(new_active, ULP_DDP_CAP_COUNT);
	const struct ulp_ddp_dev_ops *ops;
	struct ulp_ddp_dev_caps caps;
	int ret;

	ops = ctx->dev->netdev_ops->ulp_ddp_ops;
	ops->get_caps(ctx->dev, &caps);

	/* new_active = (old_active & ~req_mask) | (wanted & req_mask)
	 * new_active &= caps_hw
	 */
	bitmap_copy(old_active, caps.active, ULP_DDP_CAP_COUNT);
	bitmap_and(req_wanted, req_wanted, req_mask, ULP_DDP_CAP_COUNT);
	bitmap_andnot(new_active, old_active, req_mask, ULP_DDP_CAP_COUNT);
	bitmap_or(new_active, new_active, req_wanted, ULP_DDP_CAP_COUNT);
	bitmap_and(new_active, new_active, caps.hw, ULP_DDP_CAP_COUNT);
	if (!bitmap_equal(old_active, new_active, ULP_DDP_CAP_COUNT)) {
		ret = ops->set_caps(ctx->dev, new_active, info->extack);
		if (ret < 0)
			return ret;
		ops->get_caps(ctx->dev, &caps);
		bitmap_copy(new_active, caps.active, ULP_DDP_CAP_COUNT);
	}

	/* return 1 to notify */
	return !bitmap_equal(old_active, new_active, ULP_DDP_CAP_COUNT);
}

int ulp_ddp_nl_caps_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct ulp_ddp_reply_context *ctx = info->user_ptr[0];
	unsigned long wanted, wanted_mask;
	struct sk_buff *rsp;
	bool notify;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, ULP_DDP_A_CAPS_WANTED) ||
	    GENL_REQ_ATTR_CHECK(info, ULP_DDP_A_CAPS_WANTED_MASK))
		return -EINVAL;

	rsp = genlmsg_new(ulp_ddp_reply_size(ULP_DDP_CMD_STATS_GET), GFP_KERNEL);
	if (!rsp)
		return -EMSGSIZE;

	wanted = nla_get_u64(info->attrs[ULP_DDP_A_CAPS_WANTED]);
	wanted_mask = nla_get_u64(info->attrs[ULP_DDP_A_CAPS_WANTED_MASK]);

	ret = ulp_ddp_apply_bits(ctx, &wanted, &wanted_mask, info);
	if (ret < 0)
		goto err_rsp;

	notify = !!ret;
	ret = ulp_ddp_prepare_context(ctx, ULP_DDP_CMD_CAPS_SET);
	if (ret)
		goto err_rsp;

	ret = ulp_ddp_write_reply(rsp, ctx, ULP_DDP_CMD_CAPS_SET, info);
	if (ret < 0)
		goto err_rsp;

	ret = genlmsg_reply(rsp, info);
	if (notify)
		ulp_ddp_nl_notify_dev(ctx);

	return ret;

err_rsp:
	nlmsg_free(rsp);

	return ret;
}

int ulp_ddp_nl_stats_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct ulp_ddp_reply_context *ctx = info->user_ptr[0];
	struct sk_buff *rsp;
	int ret = 0;

	ret = ulp_ddp_prepare_context(ctx, ULP_DDP_CMD_STATS_GET);
	if (ret)
		return ret;

	rsp = genlmsg_new(ulp_ddp_reply_size(ULP_DDP_CMD_STATS_GET), GFP_KERNEL);
	if (!rsp)
		return -EMSGSIZE;

	ret = ulp_ddp_write_reply(rsp, ctx, ULP_DDP_CMD_STATS_GET, info);
	if (ret < 0)
		goto err_rsp;

	return genlmsg_reply(rsp, info);

err_rsp:
	nlmsg_free(rsp);
	return ret;
}

static int __init ulp_ddp_init(void)
{
	return genl_register_family(&ulp_ddp_nl_family);
}

subsys_initcall(ulp_ddp_init);
