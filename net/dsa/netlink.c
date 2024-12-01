// SPDX-License-Identifier: GPL-2.0
/* Copyright 2022 NXP
 */
#include <linux/netdevice.h>
#include <net/rtnetlink.h>

#include "netlink.h"
#include "user.h"

static const struct nla_policy dsa_policy[IFLA_DSA_MAX + 1] = {
	[IFLA_DSA_CONDUIT]	= { .type = NLA_U32 },
	[IFLA_DSA_FLAGS]	= { .len = sizeof(struct ifla_dsa_flags) },
};

static int dsa_dev_change_flags(const struct net_device *dev, u32 flags, u32 mask)
{
	struct dsa_user_priv *p = netdev_priv(dev);
	u32 old_flags = p->flags;

	/* For now, we only support make these changes when the port is not a member
	 * of a bridge (ie in standalone mode). If the user wants to alter these flags
	 * for ports that are currently members of a bridge need to first remove the
	 * interface from the bridge. Then they can add interface back
	 * after making their desired flag changes.
	 */

	if (netif_is_bridge_port(dev))
		return -EBUSY;

	p->flags = (old_flags & ~mask) | (flags & mask);

	return 0;
}

static int dsa_changelink(struct net_device *dev, struct nlattr *tb[],
			  struct nlattr *data[],
			  struct netlink_ext_ack *extack)
{
	int err;
	struct ifla_dsa_flags *flags;

	if (!data)
		return 0;

	if (data[IFLA_DSA_CONDUIT]) {
		u32 ifindex = nla_get_u32(data[IFLA_DSA_CONDUIT]);
		struct net_device *conduit;

		conduit = __dev_get_by_index(dev_net(dev), ifindex);
		if (!conduit)
			return -EINVAL;

		err = dsa_user_change_conduit(dev, conduit, extack);
		if (err)
			return err;
	}
	if (data[IFLA_DSA_FLAGS]) {
		flags = nla_data(data[IFLA_DSA_FLAGS]);
		err = dsa_dev_change_flags(dev, flags->flags, flags->mask);
		if (err)
			return err;
	}

	return 0;
}

static size_t dsa_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(u32)) +	/* IFLA_DSA_CONDUIT  */
	       0;
}

static int dsa_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct net_device *conduit = dsa_user_to_conduit(dev);
	struct dsa_user_priv *dsa = netdev_priv(dev);
	struct ifla_dsa_flags f;


	if (nla_put_u32(skb, IFLA_DSA_CONDUIT, conduit->ifindex))
		return -EMSGSIZE;

	if (dsa->flags) {
		f.flags = dsa->flags;
		f.mask = ~0;
		if (nla_put(skb, IFLA_DSA_FLAGS, sizeof(f), &f))
			return -EMSGSIZE;
	}

	return 0;
}

struct rtnl_link_ops dsa_link_ops __read_mostly = {
	.kind			= "dsa",
	.priv_size		= sizeof(struct dsa_port),
	.maxtype		= IFLA_DSA_MAX,
	.policy			= dsa_policy,
	.changelink		= dsa_changelink,
	.get_size		= dsa_get_size,
	.fill_info		= dsa_fill_info,
	.netns_refund		= true,
};
