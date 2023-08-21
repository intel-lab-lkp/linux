// SPDX-License-Identifier: GPL-2.0-only

#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/rtnetlink.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/netdev_rx_queue.h>

#include "netdev-genl-gen.h"

struct netdev_nl_dump_ctx {
	unsigned long	ifindex;
	int		napi_idx;
};

static inline struct netdev_nl_dump_ctx *
netdev_dump_ctx(struct netlink_callback *cb)
{
	NL_ASSERT_DUMP_CTX_FITS(struct netdev_nl_dump_ctx);

	return (struct netdev_nl_dump_ctx *)cb->ctx;
}

static int
netdev_nl_dev_fill(struct net_device *netdev, struct sk_buff *rsp,
		   const struct genl_info *info)
{
	void *hdr;

	hdr = genlmsg_iput(rsp, info);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(rsp, NETDEV_A_DEV_IFINDEX, netdev->ifindex) ||
	    nla_put_u64_64bit(rsp, NETDEV_A_DEV_XDP_FEATURES,
			      netdev->xdp_features, NETDEV_A_DEV_PAD)) {
		genlmsg_cancel(rsp, hdr);
		return -EINVAL;
	}

	if (netdev->xdp_features & NETDEV_XDP_ACT_XSK_ZEROCOPY) {
		if (nla_put_u32(rsp, NETDEV_A_DEV_XDP_ZC_MAX_SEGS,
				netdev->xdp_zc_max_segs)) {
			genlmsg_cancel(rsp, hdr);
			return -EINVAL;
		}
	}

	genlmsg_end(rsp, hdr);

	return 0;
}

static void
netdev_genl_dev_notify(struct net_device *netdev, int cmd)
{
	struct genl_info info;
	struct sk_buff *ntf;

	if (!genl_has_listeners(&netdev_nl_family, dev_net(netdev),
				NETDEV_NLGRP_MGMT))
		return;

	genl_info_init_ntf(&info, &netdev_nl_family, cmd);

	ntf = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!ntf)
		return;

	if (netdev_nl_dev_fill(netdev, ntf, &info)) {
		nlmsg_free(ntf);
		return;
	}

	genlmsg_multicast_netns(&netdev_nl_family, dev_net(netdev), ntf,
				0, NETDEV_NLGRP_MGMT, GFP_KERNEL);
}

int netdev_nl_dev_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *netdev;
	struct sk_buff *rsp;
	u32 ifindex;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, NETDEV_A_DEV_IFINDEX))
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[NETDEV_A_DEV_IFINDEX]);

	rsp = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!rsp)
		return -ENOMEM;

	rtnl_lock();

	netdev = __dev_get_by_index(genl_info_net(info), ifindex);
	if (netdev)
		err = netdev_nl_dev_fill(netdev, rsp, info);
	else
		err = -ENODEV;

	rtnl_unlock();

	if (err)
		goto err_free_msg;

	return genlmsg_reply(rsp, info);

err_free_msg:
	nlmsg_free(rsp);
	return err;
}

int netdev_nl_dev_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct netdev_nl_dump_ctx *ctx = netdev_dump_ctx(cb);
	struct net *net = sock_net(skb->sk);
	struct net_device *netdev;
	int err = 0;

	rtnl_lock();
	for_each_netdev_dump(net, netdev, ctx->ifindex) {
		err = netdev_nl_dev_fill(netdev, skb, genl_info_dump(cb));
		if (err < 0)
			break;
	}
	rtnl_unlock();

	if (err != -EMSGSIZE)
		return err;

	return skb->len;
}

static int
netdev_nl_napi_fill_one(struct sk_buff *rsp, struct napi_struct *napi,
			const struct genl_info *info)
{
	struct netdev_rx_queue *rx_queue, *rxq;
	struct netdev_queue *tx_queue, *txq;
	unsigned int rx_qid, tx_qid;
	void *hdr;

	if (!napi->dev)
		return -EINVAL;

	hdr = genlmsg_iput(rsp, info);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(rsp, NETDEV_A_NAPI_NAPI_ID, napi->napi_id))
		goto nla_put_failure;

	if (nla_put_u32(rsp, NETDEV_A_NAPI_IFINDEX, napi->dev->ifindex))
		goto nla_put_failure;

	list_for_each_entry_safe(rx_queue, rxq, &napi->napi_rxq_list, q_list) {
		rx_qid = get_netdev_rx_queue_index(rx_queue);
		if (nla_put_u32(rsp, NETDEV_A_NAPI_RX_QUEUES, rx_qid))
			goto nla_put_failure;
	}

	list_for_each_entry_safe(tx_queue, txq, &napi->napi_txq_list, q_list) {
		tx_qid = get_netdev_queue_index(tx_queue);
		if (nla_put_u32(rsp, NETDEV_A_NAPI_TX_QUEUES, tx_qid))
			goto nla_put_failure;
	}

	if (napi->irq >= 0 && (nla_put_u32(rsp, NETDEV_A_NAPI_IRQ, napi->irq)))
		goto nla_put_failure;

	genlmsg_end(rsp, hdr);

	return 0;

nla_put_failure:
	genlmsg_cancel(rsp, hdr);
	return -EMSGSIZE;
}

static int
netdev_nl_napi_fill(struct net_device *netdev, struct sk_buff *rsp,
		    const struct genl_info *info, u32 napi_id)
{
	struct napi_struct *napi, *n;

	list_for_each_entry_safe(napi, n, &netdev->napi_list, dev_list) {
		if (napi->napi_id == napi_id)
			return netdev_nl_napi_fill_one(rsp, napi, info);
	}
	return -EINVAL;
}

int netdev_nl_napi_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *netdev;
	struct sk_buff *rsp;
	u32 napi_id;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, NETDEV_A_NAPI_NAPI_ID))
		return -EINVAL;

	napi_id = nla_get_u32(info->attrs[NETDEV_A_NAPI_NAPI_ID]);

	rsp = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!rsp)
		return -ENOMEM;

	rtnl_lock();

	netdev = dev_get_by_napi_id(napi_id);
	if (netdev)
		err  = netdev_nl_napi_fill(netdev, rsp, info, napi_id);
	else
		err = -ENODEV;

	rtnl_unlock();

	if (err)
		goto err_free_msg;

	return genlmsg_reply(rsp, info);

err_free_msg:
	nlmsg_free(rsp);
	return err;
}

static int
netdev_nl_napi_dump_one(struct net_device *netdev, struct sk_buff *rsp,
			const struct genl_info *info, int *start)
{
	struct napi_struct *napi, *n;
	int err = 0;
	int i = 0;

	list_for_each_entry_safe(napi, n, &netdev->napi_list, dev_list) {
		if (i < *start) {
			i++;
			continue;
		}
		err = netdev_nl_napi_fill_one(rsp, napi, info);
		if (err)
			break;
		*start = ++i;
	}
	return err;
}

int netdev_nl_napi_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct genl_dumpit_info *info = genl_dumpit_info(cb);
	struct netdev_nl_dump_ctx *ctx = netdev_dump_ctx(cb);
	struct net *net = sock_net(skb->sk);
	struct net_device *netdev;
	int n_idx = ctx->napi_idx;
	u32 ifindex = 0;
	int err = 0;

	if (info->info.attrs[NETDEV_A_NAPI_IFINDEX])
		ifindex = nla_get_u32(info->info.attrs[NETDEV_A_NAPI_IFINDEX]);

	rtnl_lock();
	if (ifindex) {
		netdev = __dev_get_by_index(net, ifindex);
		if (netdev)
			err = netdev_nl_napi_dump_one(netdev, skb,
						      genl_info_dump(cb),
						      &n_idx);
		else
			err = -ENODEV;
	} else {
		for_each_netdev_dump(net, netdev, ctx->ifindex) {
			err = netdev_nl_napi_dump_one(netdev, skb,
						      genl_info_dump(cb),
						      &n_idx);
			if (!err)
				n_idx = 0;
			if (err < 0)
				break;
		}
	}
	rtnl_unlock();

	if (err != -EMSGSIZE)
		return err;

	ctx->napi_idx = n_idx;
	return skb->len;
}

static int netdev_genl_netdevice_event(struct notifier_block *nb,
				       unsigned long event, void *ptr)
{
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);

	switch (event) {
	case NETDEV_REGISTER:
		netdev_genl_dev_notify(netdev, NETDEV_CMD_DEV_ADD_NTF);
		break;
	case NETDEV_UNREGISTER:
		netdev_genl_dev_notify(netdev, NETDEV_CMD_DEV_DEL_NTF);
		break;
	case NETDEV_XDP_FEAT_CHANGE:
		netdev_genl_dev_notify(netdev, NETDEV_CMD_DEV_CHANGE_NTF);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block netdev_genl_nb = {
	.notifier_call	= netdev_genl_netdevice_event,
};

static int __init netdev_genl_init(void)
{
	int err;

	err = register_netdevice_notifier(&netdev_genl_nb);
	if (err)
		return err;

	err = genl_register_family(&netdev_nl_family);
	if (err)
		goto err_unreg_ntf;

	return 0;

err_unreg_ntf:
	unregister_netdevice_notifier(&netdev_genl_nb);
	return err;
}

subsys_initcall(netdev_genl_init);
