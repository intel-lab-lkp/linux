// SPDX-License-Identifier: GPL-2.0-only
#include <linux/inetdevice.h>
#include <linux/types.h>
#include <net/addrconf.h>
#include <net/lwtunnel.h>
#include <net/route.h>

static void rt_fibinfo_free(struct rtable __rcu **rtp)
{
	struct rtable *rt = rcu_dereference_protected(*rtp, 1);

	if (!rt)
		return;

	/* Not even needed : RCU_INIT_POINTER(*rtp, NULL);
	 * because we waited an RCU grace period before calling
	 * free_fib_info_rcu()
	 */

	dst_dev_put(&rt->dst);
	dst_release_immediate(&rt->dst);
}

static void free_nh_exceptions(struct fib_nh_common *nhc)
{
	struct fnhe_hash_bucket *hash;
	int i;

	hash = rcu_dereference_protected(nhc->nhc_exceptions, 1);
	if (!hash)
		return;
	for (i = 0; i < FNHE_HASH_SIZE; i++) {
		struct fib_nh_exception *fnhe;

		fnhe = rcu_dereference_protected(hash[i].chain, 1);
		while (fnhe) {
			struct fib_nh_exception *next;

			next = rcu_dereference_protected(fnhe->fnhe_next, 1);

			rt_fibinfo_free(&fnhe->fnhe_rth_input);
			rt_fibinfo_free(&fnhe->fnhe_rth_output);

			kfree(fnhe);

			fnhe = next;
		}
	}
	kfree(hash);
}

static void rt_fibinfo_free_cpus(struct rtable __rcu * __percpu *rtp)
{
	int cpu;

	if (!rtp)
		return;

	for_each_possible_cpu(cpu) {
		struct rtable *rt;

		rt = rcu_dereference_protected(*per_cpu_ptr(rtp, cpu), 1);
		if (rt) {
			dst_dev_put(&rt->dst);
			dst_release_immediate(&rt->dst);
		}
	}
	free_percpu(rtp);
}

void fib_nh_common_release(struct fib_nh_common *nhc)
{
	netdev_put(nhc->nhc_dev, &nhc->nhc_dev_tracker);
	lwtstate_put(nhc->nhc_lwtstate);
	rt_fibinfo_free_cpus(nhc->nhc_pcpu_rth_output);
	rt_fibinfo_free(&nhc->nhc_rth_input);
	free_nh_exceptions(nhc);
}

int fib_nh_common_init(struct net *net, struct fib_nh_common *nhc,
		       struct nlattr *encap, u16 encap_type,
		       void *cfg, gfp_t gfp_flags,
		       struct netlink_ext_ack *extack)
{
	int err;

	nhc->nhc_pcpu_rth_output = alloc_percpu_gfp(struct rtable __rcu *,
						    gfp_flags);
	if (!nhc->nhc_pcpu_rth_output)
		return -ENOMEM;

	if (encap) {
		struct lwtunnel_state *lwtstate;

		err = lwtunnel_build_state(net, encap_type, encap,
					   nhc->nhc_family, cfg, &lwtstate,
					   extack);
		if (err)
			goto lwt_failure;

		nhc->nhc_lwtstate = lwtstate_get(lwtstate);
	}

	return 0;

lwt_failure:
	rt_fibinfo_free_cpus(nhc->nhc_pcpu_rth_output);
	nhc->nhc_pcpu_rth_output = NULL;
	return err;
}

int fib_nexthop_info(struct sk_buff *skb, const struct fib_nh_common *nhc,
		     u8 rt_family, unsigned char *flags, bool skip_oif)
{
	if (nhc->nhc_flags & RTNH_F_DEAD)
		*flags |= RTNH_F_DEAD;

	if (nhc->nhc_flags & RTNH_F_LINKDOWN) {
		*flags |= RTNH_F_LINKDOWN;

		rcu_read_lock();
		switch (nhc->nhc_family) {
		case AF_INET:
			if (ip_ignore_linkdown(nhc->nhc_dev))
				*flags |= RTNH_F_DEAD;
			break;
		case AF_INET6:
			if (ip6_ignore_linkdown(nhc->nhc_dev))
				*flags |= RTNH_F_DEAD;
			break;
		}
		rcu_read_unlock();
	}

	switch (nhc->nhc_gw_family) {
	case AF_INET:
		if (nla_put_in_addr(skb, RTA_GATEWAY, nhc->nhc_gw.ipv4))
			goto nla_put_failure;
		break;
	case AF_INET6:
		/* if gateway family does not match nexthop family
		 * gateway is encoded as RTA_VIA
		 */
		if (rt_family != nhc->nhc_gw_family) {
			int alen = sizeof(struct in6_addr);
			struct nlattr *nla;
			struct rtvia *via;

			nla = nla_reserve(skb, RTA_VIA, alen + 2);
			if (!nla)
				goto nla_put_failure;

			via = nla_data(nla);
			via->rtvia_family = AF_INET6;
			memcpy(via->rtvia_addr, &nhc->nhc_gw.ipv6, alen);
		} else if (nla_put_in6_addr(skb, RTA_GATEWAY,
					    &nhc->nhc_gw.ipv6) < 0) {
			goto nla_put_failure;
		}
		break;
	}

	*flags |= (nhc->nhc_flags &
		   (RTNH_F_ONLINK | RTNH_F_OFFLOAD | RTNH_F_TRAP));

	if (!skip_oif && nhc->nhc_dev &&
	    nla_put_u32(skb, RTA_OIF, nhc->nhc_dev->ifindex))
		goto nla_put_failure;

	if (lwtunnel_fill_encap(skb, nhc->nhc_lwtstate,
				RTA_ENCAP, RTA_ENCAP_TYPE) < 0)
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

#if IS_ENABLED(CONFIG_IP_ROUTE_MULTIPATH) || IS_ENABLED(CONFIG_IPV6)
int fib_add_nexthop(struct sk_buff *skb, const struct fib_nh_common *nhc,
		    int nh_weight, u8 rt_family, u32 nh_tclassid)
{
	const struct net_device *dev = nhc->nhc_dev;
	struct rtnexthop *rtnh;
	unsigned char flags = 0;

	rtnh = nla_reserve_nohdr(skb, sizeof(*rtnh));
	if (!rtnh)
		goto nla_put_failure;

	rtnh->rtnh_hops = nh_weight - 1;
	rtnh->rtnh_ifindex = dev ? dev->ifindex : 0;

	if (fib_nexthop_info(skb, nhc, rt_family, &flags, true) < 0)
		goto nla_put_failure;

	rtnh->rtnh_flags = flags;

	if (nh_tclassid && nla_put_u32(skb, RTA_FLOW, nh_tclassid))
		goto nla_put_failure;

	/* length of rtnetlink header + attributes */
	rtnh->rtnh_len = nlmsg_get_pos(skb) - (void *)rtnh;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}
#endif

const struct nla_policy rtm_ipv4_policy[RTA_MAX + 1] = {
	[RTA_UNSPEC]		= { .strict_start_type = RTA_DPORT + 1 },
	[RTA_DST]		= { .type = NLA_U32 },
	[RTA_SRC]		= { .type = NLA_U32 },
	[RTA_IIF]		= { .type = NLA_U32 },
	[RTA_OIF]		= { .type = NLA_U32 },
	[RTA_GATEWAY]		= { .type = NLA_U32 },
	[RTA_PRIORITY]		= { .type = NLA_U32 },
	[RTA_PREFSRC]		= { .type = NLA_U32 },
	[RTA_METRICS]		= { .type = NLA_NESTED },
	[RTA_MULTIPATH]		= { .len = sizeof(struct rtnexthop) },
	[RTA_FLOW]		= { .type = NLA_U32 },
	[RTA_ENCAP_TYPE]	= { .type = NLA_U16 },
	[RTA_ENCAP]		= { .type = NLA_NESTED },
	[RTA_UID]		= { .type = NLA_U32 },
	[RTA_MARK]		= { .type = NLA_U32 },
	[RTA_TABLE]		= { .type = NLA_U32 },
	[RTA_IP_PROTO]		= { .type = NLA_U8 },
	[RTA_SPORT]		= { .type = NLA_U16 },
	[RTA_DPORT]		= { .type = NLA_U16 },
	[RTA_NH_ID]		= { .type = NLA_U32 },
};

int ip_valid_fib_dump_req(struct net *net, const struct nlmsghdr *nlh,
			  struct fib_dump_filter *filter,
			  struct netlink_callback *cb)
{
	struct netlink_ext_ack *extack = cb->extack;
	struct nlattr *tb[RTA_MAX + 1];
	struct rtmsg *rtm;
	int err, i;

	rtm = nlmsg_payload(nlh, sizeof(*rtm));
	if (!rtm) {
		NL_SET_ERR_MSG(extack, "Invalid header for FIB dump request");
		return -EINVAL;
	}

	if (rtm->rtm_dst_len || rtm->rtm_src_len  || rtm->rtm_tos   ||
	    rtm->rtm_scope) {
		NL_SET_ERR_MSG(extack, "Invalid values in header for FIB dump request");
		return -EINVAL;
	}

	if (rtm->rtm_flags & ~(RTM_F_CLONED | RTM_F_PREFIX)) {
		NL_SET_ERR_MSG(extack, "Invalid flags for FIB dump request");
		return -EINVAL;
	}
	if (rtm->rtm_flags & RTM_F_CLONED)
		filter->dump_routes = false;
	else
		filter->dump_exceptions = false;

	filter->flags    = rtm->rtm_flags;
	filter->protocol = rtm->rtm_protocol;
	filter->rt_type  = rtm->rtm_type;
	filter->table_id = rtm->rtm_table;

	err = nlmsg_parse_deprecated_strict(nlh, sizeof(*rtm), tb, RTA_MAX,
					    rtm_ipv4_policy, extack);
	if (err < 0)
		return err;

	for (i = 0; i <= RTA_MAX; ++i) {
		int ifindex;

		if (!tb[i])
			continue;

		switch (i) {
		case RTA_TABLE:
			filter->table_id = nla_get_u32(tb[i]);
			break;
		case RTA_OIF:
			ifindex = nla_get_u32(tb[i]);

			filter->dev = dev_get_by_index_rcu(net, ifindex);
			if (!filter->dev)
				return -ENODEV;
			break;
		default:
			NL_SET_ERR_MSG(extack, "Unsupported attribute in dump request");
			return -EINVAL;
		}
	}

	if (filter->flags || filter->protocol || filter->rt_type ||
	    filter->table_id || filter->dev) {
		filter->filter_set = 1;
		cb->answer_flags = NLM_F_DUMP_FILTERED;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ip_valid_fib_dump_req);
