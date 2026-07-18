// SPDX-License-Identifier: GPL-2.0
/*
 * GeoNetworking Netlink interface
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>
#include <linux/if_arp.h>
#include <linux/slab.h>
#include <linux/rculist.h>
#include <linux/gn.h>
#include <linux/gn_routing.h>
#include <net/sock.h>

static const struct nla_policy ifa_gn_policy[IFA_MAX + 1] = {
	[IFA_ADDRESS]		= { .type = NLA_U64 },
	[IFA_LOCAL]		= { .type = NLA_U64 },
};

static int gn_rtm_newaddr(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct nlattr *tb[IFA_MAX + 1];
	struct net_device *dev;
	struct sockaddr_gn sa;
	struct gn_iface *gnif;
	struct ifaddrmsg *ifm;
	gn_address_t *addr;
	int rc;

	rc = nlmsg_parse(nlh, sizeof(*ifm), tb, IFA_MAX, ifa_gn_policy, extack);
	if (rc < 0)
		return rc;

	ifm = nlmsg_data(nlh);
	if (ifm->ifa_family != AF_GN)
		return -EPFNOSUPPORT;

	if (tb[IFA_LOCAL])
		addr = nla_data(tb[IFA_LOCAL]);
	else if (tb[IFA_ADDRESS])
		addr = nla_data(tb[IFA_ADDRESS]);
	else
		return -EINVAL;

	dev = __dev_get_by_index(net, ifm->ifa_index);
	if (!dev)
		return -ENODEV;

	if (dev->type != ARPHRD_ETHER)
		return -EINVAL;

	if (gn_find_interface_by_dev(dev))
		return -EEXIST;

	memset(&sa, 0, sizeof(sa));
	sa.sgn_family = AF_GN;
	sa.sgn_addr = *addr;
	sa.sgn_port = GNPORT_ANY;

	gnif = gn_if_add_device(dev, &sa);
	if (!gnif)
		return -ENOMEM;

	return 0;
}

static int gn_rtm_deladdr(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct nlattr *tb[IFA_MAX + 1];
	struct net_device *dev;
	struct ifaddrmsg *ifm;
	int rc;

	rc = nlmsg_parse(nlh, sizeof(*ifm), tb, IFA_MAX, ifa_gn_policy, extack);
	if (rc < 0)
		return rc;

	ifm = nlmsg_data(nlh);
	if (ifm->ifa_family != AF_GN)
		return -EPFNOSUPPORT;

	dev = __dev_get_by_index(net, ifm->ifa_index);
	if (!dev)
		return -ENODEV;

	if (!gn_find_interface_by_dev(dev))
		return -EADDRNOTAVAIL;

	gn_if_drop_device(dev);
	return 0;
}

static int gn_fill_addrinfo(struct sk_buff *skb, struct gn_iface *gnif,
			    int msg_type, u32 portid, u32 seq, int flag)
{
	struct ifaddrmsg *hdr;
	struct nlmsghdr *nlh;

	nlh = nlmsg_put(skb, portid, seq, msg_type, sizeof(*hdr), flag);
	if (!nlh)
		return -EMSGSIZE;

	hdr = nlmsg_data(nlh);
	memset(hdr, 0, sizeof(*hdr));
	hdr->ifa_family = AF_GN;
	hdr->ifa_prefixlen = 0;
	hdr->ifa_flags = 0;
	hdr->ifa_scope = 0;
	hdr->ifa_index = gnif->dev->ifindex;

	if (nla_put_u64_64bit(skb, IFA_LOCAL, gnif->address, IFA_UNSPEC) ||
	    nla_put_u64_64bit(skb, IFA_ADDRESS, gnif->address, IFA_UNSPEC)) {
		nlmsg_cancel(skb, nlh);
		return -EMSGSIZE;
	}

	nlmsg_end(skb, nlh);
	return 0;
}

static int gn_dump_addrinfo(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct ifaddrmsg *hdr;
	struct gn_iface *gnif;
	int ifindex = 0;
	int idx = 0;
	int s_idx = cb->args[0];
	int rc = 0;

	hdr = nlmsg_payload(cb->nlh, sizeof(*hdr));
	if (hdr)
		ifindex = hdr->ifa_index;

	rcu_read_lock();
	hlist_for_each_entry_rcu(gnif, &gn_interfaces, hnode) {
		if (!net_eq(dev_net(gnif->dev), net))
			continue;
		if (ifindex && ifindex != gnif->dev->ifindex)
			continue;
		if (idx < s_idx) {
			idx++;
			continue;
		}
		rc = gn_fill_addrinfo(skb, gnif, RTM_NEWADDR,
				      NETLINK_CB(cb->skb).portid,
				      cb->nlh->nlmsg_seq, NLM_F_MULTI);
		if (rc < 0)
			break;
		idx++;
	}
	rcu_read_unlock();

	cb->args[0] = idx;
	return skb->len;
}

static const struct nla_policy ifla_gn_policy[IFLA_GN_MAX + 1] = {
	[IFLA_GN_POSITION]	= NLA_POLICY_EXACT_LEN(sizeof(struct gn_position)),
};

static int gn_fill_link_af(struct sk_buff *skb, const struct net_device *dev,
			   u32 ext_filter_mask)
{
	struct gn_iface *gnif;

	gnif = gn_find_interface_by_dev((struct net_device *)dev);
	if (!gnif)
		return -ENODATA;

	if (nla_put(skb, IFLA_GN_POSITION, sizeof(gnif->pos), &gnif->pos))
		return -EMSGSIZE;

	return 0;
}

static size_t gn_get_link_af_size(const struct net_device *dev,
				  u32 ext_filter_mask)
{
	struct gn_iface *gnif;

	gnif = gn_find_interface_by_dev((struct net_device *)dev);
	if (!gnif)
		return 0;

	return nla_total_size(sizeof(struct gn_position));
}

static int gn_set_link_af(struct net_device *dev, const struct nlattr *attr,
			  struct netlink_ext_ack *extack)
{
	struct nlattr *tb[IFLA_GN_MAX + 1];
	struct gn_position pos;
	struct gn_iface *gnif;
	int rc;

	rc = nla_parse_nested(tb, IFLA_GN_MAX, attr, ifla_gn_policy, extack);
	if (rc < 0)
		return rc;

	if (!tb[IFLA_GN_POSITION])
		return 0;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (nla_len(tb[IFLA_GN_POSITION]) < sizeof(struct gn_position))
		return -EINVAL;

	memcpy(&pos, nla_data(tb[IFLA_GN_POSITION]), sizeof(pos));
	rc = gn_validate_pos(&pos);
	if (rc < 0)
		return rc;

	gnif = gn_find_interface_by_dev(dev);
	if (!gnif)
		return -EADDRNOTAVAIL;

	memcpy(&gnif->pos, &pos, sizeof(pos));
	return 0;
}

static struct rtnl_af_ops gn_af_ops __read_mostly = {
	.family			= PF_GN,
	.fill_link_af		= gn_fill_link_af,
	.get_link_af_size	= gn_get_link_af_size,
	.set_link_af		= gn_set_link_af,
};

static const struct rtnl_msg_handler gn_rtnl_msg_handlers[] = {
	{ .owner = THIS_MODULE, .protocol = PF_GN, .msgtype = RTM_NEWADDR,
	  .doit = gn_rtm_newaddr },
	{ .owner = THIS_MODULE, .protocol = PF_GN, .msgtype = RTM_DELADDR,
	  .doit = gn_rtm_deladdr },
	{ .owner = THIS_MODULE, .protocol = PF_GN, .msgtype = RTM_GETADDR,
	  .dumpit = gn_dump_addrinfo },
};

int __init gn_netlink_init(void)
{
	int err;

	err = rtnl_af_register(&gn_af_ops);
	if (err)
		return err;

	err = rtnl_register_many(gn_rtnl_msg_handlers);
	if (err)
		rtnl_af_unregister(&gn_af_ops);

	return err;
}

void gn_netlink_exit(void)
{
	rtnl_unregister_many(gn_rtnl_msg_handlers);
	rtnl_af_unregister(&gn_af_ops);
}
