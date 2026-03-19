// SPDX-License-Identifier: GPL-2.0
/*  IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
 *
 *  Copyright (C) 2026- Mandelbit SRL
 *  Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
 *
 *  Author:	Antonio Quartulli <antonio@mandelbit.com>
 *		Daniel Gröber <dxld@darkboxed.org>
 *		Ralf Lici <ralf@mandelbit.com>
 */

#include <net/genetlink.h>
#include <net/ipv6.h>

#include <uapi/linux/ipxlat.h>

#include "netlink.h"
#include "main.h"
#include "netlink-gen.h"
#include "ipxlpriv.h"

MODULE_ALIAS_GENL_FAMILY(IPXLAT_FAMILY_NAME);

struct ipxlat_nl_info_ctx {
	struct ipxlat_priv *ipxlat;
	netdevice_tracker tracker;
};

struct ipxlat_nl_dump_ctx {
	unsigned long last_ifindex;
};

/**
 * ipxlat_get_from_attrs - retrieve ipxlat private data for target netdev
 * @net: network namespace where to look for the interface
 * @info: generic netlink info from the user request
 * @tracker: tracker object to be used for the netdev reference acquisition
 *
 * Return: the ipxlat private data, if found, or an error otherwise
 */
static struct ipxlat_priv *ipxlat_get_from_attrs(struct net *net,
						 struct genl_info *info,
						 netdevice_tracker *tracker)
{
	struct ipxlat_priv *ipxlat;
	struct net_device *dev;
	int ifindex;

	if (GENL_REQ_ATTR_CHECK(info, IPXLAT_A_DEV_IFINDEX))
		return ERR_PTR(-EINVAL);
	ifindex = nla_get_u32(info->attrs[IPXLAT_A_DEV_IFINDEX]);

	rcu_read_lock();
	dev = dev_get_by_index_rcu(net, ifindex);
	if (!dev) {
		rcu_read_unlock();
		NL_SET_ERR_MSG_MOD(info->extack,
				   "ifindex does not match any interface");
		return ERR_PTR(-ENODEV);
	}

	if (!ipxlat_dev_is_valid(dev)) {
		rcu_read_unlock();
		NL_SET_ERR_MSG_MOD(info->extack,
				   "specified interface is not ipxlat");
		NL_SET_BAD_ATTR(info->extack,
				info->attrs[IPXLAT_A_DEV_IFINDEX]);
		return ERR_PTR(-EINVAL);
	}

	ipxlat = netdev_priv(dev);
	netdev_hold(dev, tracker, GFP_ATOMIC);
	rcu_read_unlock();

	return ipxlat;
}

int ipxlat_nl_pre_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
		       struct genl_info *info)
{
	struct ipxlat_nl_info_ctx *ctx = (struct ipxlat_nl_info_ctx *)info->ctx;
	struct ipxlat_priv *ipxlat;

	BUILD_BUG_ON(sizeof(*ctx) > sizeof(info->ctx));

	ipxlat = ipxlat_get_from_attrs(genl_info_net(info), info,
				       &ctx->tracker);
	if (IS_ERR(ipxlat))
		return PTR_ERR(ipxlat);

	ctx->ipxlat = ipxlat;
	return 0;
}

void ipxlat_nl_post_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
			 struct genl_info *info)
{
	struct ipxlat_nl_info_ctx *ctx = (struct ipxlat_nl_info_ctx *)info->ctx;

	if (ctx->ipxlat)
		netdev_put(ctx->ipxlat->dev, &ctx->tracker);
}

static int ipxlat_nl_send_dev(struct sk_buff *skb, struct ipxlat_priv *ipxlat,
			      struct net *src_net, const u32 portid,
			      const u32 seq, int flags)
{
	struct nlattr *attr_cfg, *attr_pool;
	struct ipv6_prefix xlat_prefix6;
	int id, ret = -EMSGSIZE;
	u32 lowest_ipv6_mtu;
	void *hdr;

	/* snapshot settings under lock so userspace sees a coherent state */
	mutex_lock(&ipxlat->cfg_lock);
	xlat_prefix6 = ipxlat->xlat_prefix6;
	lowest_ipv6_mtu = ipxlat->lowest_ipv6_mtu;
	mutex_unlock(&ipxlat->cfg_lock);

	hdr = genlmsg_put(skb, portid, seq, &ipxlat_nl_family, flags,
			  IPXLAT_CMD_DEV_GET);
	if (!hdr)
		return -ENOBUFS;

	if (nla_put_u32(skb, IPXLAT_A_DEV_IFINDEX, ipxlat->dev->ifindex))
		goto err;

	if (!net_eq(src_net, dev_net(ipxlat->dev))) {
		id = peernet2id_alloc(src_net, dev_net(ipxlat->dev),
				      GFP_ATOMIC);
		if (id < 0) {
			ret = id;
			goto err;
		}
		if (nla_put_s32(skb, IPXLAT_A_DEV_NETNSID, id))
			goto err;
	}

	attr_cfg = nla_nest_start(skb, IPXLAT_A_DEV_CONFIG);
	if (!attr_cfg)
		goto err;

	attr_pool = nla_nest_start(skb, IPXLAT_A_CFG_XLAT_PREFIX6);
	if (!attr_pool)
		goto err;

	if (nla_put_in6_addr(skb, IPXLAT_A_POOL_PREFIX, &xlat_prefix6.addr) ||
	    nla_put_u8(skb, IPXLAT_A_POOL_PREFIX_LEN, xlat_prefix6.len))
		goto err;

	nla_nest_end(skb, attr_pool);

	if (nla_put_u32(skb, IPXLAT_A_CFG_LOWEST_IPV6_MTU, lowest_ipv6_mtu))
		goto err;

	nla_nest_end(skb, attr_cfg);
	genlmsg_end(skb, hdr);

	return 0;
err:
	genlmsg_cancel(skb, hdr);
	return ret;
}

int ipxlat_nl_dev_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct ipxlat_nl_info_ctx *ctx = (struct ipxlat_nl_info_ctx *)info->ctx;
	struct sk_buff *reply;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, IPXLAT_A_DEV_IFINDEX))
		return -EINVAL;

	reply = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!reply)
		return -ENOMEM;

	ret = ipxlat_nl_send_dev(reply, ctx->ipxlat, genl_info_net(info),
				 info->snd_portid, info->snd_seq, 0);
	if (ret < 0) {
		nlmsg_free(reply);
		return ret;
	}

	return genlmsg_reply(reply, info);
}

int ipxlat_nl_dev_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ipxlat_nl_dump_ctx *state = (struct ipxlat_nl_dump_ctx *)cb->ctx;
	struct net *net = sock_net(cb->skb->sk);
	netdevice_tracker tracker;
	struct net_device *dev;
	int ret;

	rcu_read_lock();
	for_each_netdev_dump(net, dev, state->last_ifindex) {
		if (!ipxlat_dev_is_valid(dev))
			continue;

		netdev_hold(dev, &tracker, GFP_ATOMIC);
		rcu_read_unlock();

		ret = ipxlat_nl_send_dev(skb, netdev_priv(dev), net,
					 NETLINK_CB(cb->skb).portid,
				       cb->nlh->nlmsg_seq, NLM_F_MULTI);

		rcu_read_lock();
		netdev_put(dev, &tracker);

		if (ret < 0) {
			if (skb->len > 0)
				break;
			rcu_read_unlock();
			return ret;
		}
	}
	rcu_read_unlock();
	return skb->len;
}

static int ipxlat_nl_validate_xlat_prefix6(const struct ipv6_prefix *prefix,
					   struct netlink_ext_ack *extack)
{
	if (prefix->len != 32 && prefix->len != 40 && prefix->len != 48 &&
	    prefix->len != 56 && prefix->len != 64 && prefix->len != 96) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "unsupported RFC 6052 prefix length: %u",
				       prefix->len);
		return -EINVAL;
	}

	return 0;
}

static int ipxlat_nl_parse_xlat_prefix6(struct nlattr *attr,
					struct ipv6_prefix *xlat_prefix6,
					struct netlink_ext_ack *extack)
{
	struct nlattr *attrs_pool[IPXLAT_A_POOL_MAX + 1];
	struct ipv6_prefix new_xlat_prefix6;
	int ret;

	new_xlat_prefix6 = *xlat_prefix6;

	ret = nla_parse_nested(attrs_pool, IPXLAT_A_POOL_MAX, attr,
			       ipxlat_pool_nl_policy, extack);
	if (ret)
		return ret;

	if (!attrs_pool[IPXLAT_A_POOL_PREFIX] &&
	    !attrs_pool[IPXLAT_A_POOL_PREFIX_LEN]) {
		NL_SET_ERR_MSG_MOD(extack, "xlat-prefix6 update is empty");
		return -EINVAL;
	}

	if (attrs_pool[IPXLAT_A_POOL_PREFIX])
		new_xlat_prefix6.addr =
			nla_get_in6_addr(attrs_pool[IPXLAT_A_POOL_PREFIX]);
	if (attrs_pool[IPXLAT_A_POOL_PREFIX_LEN])
		new_xlat_prefix6.len =
			nla_get_u8(attrs_pool[IPXLAT_A_POOL_PREFIX_LEN]);

	ret = ipxlat_nl_validate_xlat_prefix6(&new_xlat_prefix6, extack);
	if (ret) {
		if (attrs_pool[IPXLAT_A_POOL_PREFIX_LEN])
			NL_SET_BAD_ATTR(extack,
					attrs_pool[IPXLAT_A_POOL_PREFIX_LEN]);
		else
			NL_SET_BAD_ATTR(extack,
					attrs_pool[IPXLAT_A_POOL_PREFIX]);
		return ret;
	}

	*xlat_prefix6 = new_xlat_prefix6;
	return 0;
}

int ipxlat_nl_dev_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct ipxlat_nl_info_ctx *ctx = (struct ipxlat_nl_info_ctx *)info->ctx;
	struct nlattr *attrs[IPXLAT_A_CFG_MAX + 1];
	struct nlattr *xlat_prefix6_attr;
	struct ipv6_prefix xlat_prefix6;
	u32 lowest_ipv6_mtu;
	int ret = 0;

	if (GENL_REQ_ATTR_CHECK(info, IPXLAT_A_DEV_CONFIG))
		return -EINVAL;

	ret = nla_parse_nested(attrs, IPXLAT_A_CFG_MAX,
			       info->attrs[IPXLAT_A_DEV_CONFIG],
			       ipxlat_cfg_nl_policy, info->extack);
	if (ret)
		return ret;

	if (!attrs[IPXLAT_A_CFG_XLAT_PREFIX6] &&
	    !attrs[IPXLAT_A_CFG_LOWEST_IPV6_MTU]) {
		NL_SET_ERR_MSG_MOD(info->extack, "config update is empty");
		return -EINVAL;
	}
	xlat_prefix6_attr = attrs[IPXLAT_A_CFG_XLAT_PREFIX6];

	mutex_lock(&ctx->ipxlat->cfg_lock);

	/* Stage updates that can fail before writing device state.
	 * This keeps dev-set all-or-nothing and avoids partial commits when
	 * xlat-prefix parsing/validation fails.
	 */
	if (xlat_prefix6_attr) {
		xlat_prefix6 = ctx->ipxlat->xlat_prefix6;
		ret = ipxlat_nl_parse_xlat_prefix6(xlat_prefix6_attr,
						   &xlat_prefix6,
						   info->extack);
		if (ret)
			goto out_unlock;
	}

	if (xlat_prefix6_attr)
		ctx->ipxlat->xlat_prefix6 = xlat_prefix6;
	if (attrs[IPXLAT_A_CFG_LOWEST_IPV6_MTU]) {
		lowest_ipv6_mtu =
			nla_get_u32(attrs[IPXLAT_A_CFG_LOWEST_IPV6_MTU]);
		WRITE_ONCE(ctx->ipxlat->lowest_ipv6_mtu, lowest_ipv6_mtu);
	}

out_unlock:
	mutex_unlock(&ctx->ipxlat->cfg_lock);
	return ret;
}

/**
 * ipxlat_nl_register - perform any needed registration in the netlink subsystem
 *
 * Return: 0 on success, a negative error code otherwise
 */
int __init ipxlat_nl_register(void)
{
	return genl_register_family(&ipxlat_nl_family);
}

/**
 * ipxlat_nl_unregister - undo any module wide netlink registration
 */
void ipxlat_nl_unregister(void)
{
	genl_unregister_family(&ipxlat_nl_family);
}
