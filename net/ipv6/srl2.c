// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  SRv6 L2 tunnel device (srl2)
 *
 *  A virtual Ethernet device that encapsulates L2 frames in IPv6 with a
 *  Segment Routing Header (SRH) for transmission over an SRv6 network.
 *  On the remote side, a seg6_local behavior such as End.DT2U or End.DX2
 *  decapsulates the inner Ethernet frame for L2 delivery.
 *
 *  The encapsulation logic reuses seg6_do_srh_encap() from seg6_iptunnel.c
 *  with IPPROTO_ETHERNET (143). The transmit path uses the standard IPv6
 *  tunnel infrastructure (dst_cache, ip6_route_output, ip6tunnel_xmit).
 *
 *  Authors:
 *	Andrea Mayer <andrea.mayer@uniroma2.it>
 *	Stefano Salsano <stefano.salsano@uniroma2.it>
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/dst_cache.h>
#include <net/ip6_route.h>
#include <net/ip_tunnels.h>
#include <net/ip6_tunnel.h>
#include <net/seg6.h>
#include <linux/seg6.h>
#include <linux/srl2.h>

/* Conservative initial estimate for SRH size before newlink provides
 * the actual value. 256 bytes accommodates up to 15 SIDs.
 */
#define SRL2_SRH_HEADROOM_EST	256

struct srl2_priv {
	struct ipv6_sr_hdr	*srh;
	struct dst_cache	dst_cache;
};

/*
 * srl2_xmit - encapsulate an L2 frame in IPv6+SRH and transmit
 *
 * When the bridge (or local stack) sends a frame through this device,
 * skb->data points to the inner Ethernet header.  We look up a route
 * towards the first SID, prepend the outer IPv6+SRH via
 * seg6_do_srh_encap(), and transmit via ip6tunnel_xmit().
 *
 * The route lookup result is cached per-cpu in dst_cache. Since the
 * first SID is constant for the lifetime of the device, the cache
 * avoids repeated route lookups in the common case.
 */
static netdev_tx_t srl2_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct srl2_priv *priv = netdev_priv(dev);
	struct net *net = dev_net(dev);
	struct dst_entry *dst;
	struct flowi6 fl6;
	int err;

	local_bh_disable();
	dst = dst_cache_get(&priv->dst_cache);
	local_bh_enable();

	if (unlikely(!dst)) {
		memset(&fl6, 0, sizeof(fl6));
		fl6.daddr = priv->srh->segments[priv->srh->first_segment];

		dst = ip6_route_output(net, NULL, &fl6);
		if (dst->error) {
			dst_release(dst);
			DEV_STATS_INC(dev, tx_carrier_errors);
			goto drop;
		}

		if (dst_dev(dst) == dev) {
			dst_release(dst);
			DEV_STATS_INC(dev, collisions);
			goto drop;
		}

		local_bh_disable();
		/* saddr is unused */
		dst_cache_set_ip6(&priv->dst_cache, dst, &fl6.saddr);
		local_bh_enable();
	}

	skb_scrub_packet(skb, false);

	skb_dst_set(skb, dst);

	err = seg6_do_srh_encap(skb, priv->srh, IPPROTO_ETHERNET);
	if (unlikely(err)) {
		DEV_STATS_INC(dev, tx_errors);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	skb->protocol = htons(ETH_P_IPV6);

	ip6tunnel_xmit(NULL, skb, dev, 0);

	return NETDEV_TX_OK;

drop:
	DEV_STATS_INC(dev, tx_dropped);
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int srl2_dev_init(struct net_device *dev)
{
	struct srl2_priv *priv = netdev_priv(dev);

	return dst_cache_init(&priv->dst_cache, GFP_KERNEL);
}

static void srl2_dev_uninit(struct net_device *dev)
{
	struct srl2_priv *priv = netdev_priv(dev);

	dst_cache_destroy(&priv->dst_cache);
}

static void srl2_dev_free(struct net_device *dev)
{
	struct srl2_priv *priv = netdev_priv(dev);

	kfree(priv->srh);
}

static const struct net_device_ops srl2_netdev_ops = {
	.ndo_init		= srl2_dev_init,
	.ndo_uninit		= srl2_dev_uninit,
	.ndo_start_xmit		= srl2_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static void srl2_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &srl2_netdev_ops;
	dev->needs_free_netdev = true;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
	dev->needed_headroom = LL_MAX_HEADER + sizeof(struct ipv6hdr) +
			       SRL2_SRH_HEADROOM_EST;

	dev->priv_flags &= ~IFF_TX_SKB_SHARING;
	dev->priv_flags |= IFF_LIVE_ADDR_CHANGE | IFF_NO_QUEUE;
	dev->lltx = true;

	eth_hw_addr_random(dev);
}

static const struct nla_policy srl2_policy[IFLA_SRL2_MAX + 1] = {
	[IFLA_SRL2_SRH]	= { .type = NLA_BINARY },
};

static int srl2_validate(struct nlattr *tb[], struct nlattr *data[],
			 struct netlink_ext_ack *extack)
{
	if (!data || !data[IFLA_SRL2_SRH]) {
		NL_SET_ERR_MSG(extack, "SRH with segment list is required");
		return -EINVAL;
	}

	return 0;
}

static int srl2_newlink(struct net_device *dev,
			struct rtnl_newlink_params *params,
			struct netlink_ext_ack *extack)
{
	struct srl2_priv *priv = netdev_priv(dev);
	struct nlattr **data = params->data;
	struct ipv6_sr_hdr *srh;
	int srhlen;
	int len;

	srh = nla_data(data[IFLA_SRL2_SRH]);
	len = nla_len(data[IFLA_SRL2_SRH]);

	if (len < sizeof(*srh) + sizeof(struct in6_addr)) {
		NL_SET_ERR_MSG(extack, "SRH too short");
		return -EINVAL;
	}

	if (!seg6_validate_srh(srh, len, false)) {
		NL_SET_ERR_MSG(extack, "Invalid SRH");
		return -EINVAL;
	}

	priv->srh = kmemdup(srh, len, GFP_KERNEL);
	if (!priv->srh)
		return -ENOMEM;

	srhlen = ipv6_optlen(srh);

	dev->needed_headroom = LL_MAX_HEADER + sizeof(struct ipv6hdr) + srhlen;

	/* dev->mtu is the inner L3 payload size. Since SRv6 encapsulation
	 * carries the full inner Ethernet frame, subtract both the outer
	 * IPv6+SRH overhead and ETH_HLEN from ETH_DATA_LEN.
	 */
	dev->mtu = ETH_DATA_LEN - sizeof(struct ipv6hdr) - srhlen - ETH_HLEN;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = IP_MAX_MTU - sizeof(struct ipv6hdr) - srhlen - ETH_HLEN;

	dev->priv_destructor = srl2_dev_free;

	return register_netdevice(dev);
}

static void srl2_dellink(struct net_device *dev, struct list_head *head)
{
	unregister_netdevice_queue(dev, head);
}

static size_t srl2_get_size(const struct net_device *dev)
{
	const struct srl2_priv *priv = netdev_priv(dev);
	int srhlen = ipv6_optlen(priv->srh);

	return nla_total_size(srhlen);
}

static int srl2_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	const struct srl2_priv *priv = netdev_priv(dev);
	int srhlen = ipv6_optlen(priv->srh);

	if (nla_put(skb, IFLA_SRL2_SRH, srhlen, priv->srh))
		return -EMSGSIZE;

	return 0;
}

static struct rtnl_link_ops srl2_link_ops __read_mostly = {
	.kind		= "srl2",
	.maxtype	= IFLA_SRL2_MAX,
	.policy		= srl2_policy,
	.priv_size	= sizeof(struct srl2_priv),
	.setup		= srl2_setup,
	.validate	= srl2_validate,
	.newlink	= srl2_newlink,
	.dellink	= srl2_dellink,
	.get_size	= srl2_get_size,
	.fill_info	= srl2_fill_info,
};

static int __init srl2_init(void)
{
	return rtnl_link_register(&srl2_link_ops);
}

static void __exit srl2_exit(void)
{
	rtnl_link_unregister(&srl2_link_ops);
}

module_init(srl2_init);
module_exit(srl2_exit);

MODULE_AUTHOR("Andrea Mayer <andrea.mayer@uniroma2.it>");
MODULE_AUTHOR("Stefano Salsano <stefano.salsano@uniroma2.it>");
MODULE_DESCRIPTION("SRv6 L2 tunnel device");
MODULE_LICENSE("GPL");
MODULE_ALIAS_RTNL_LINK("srl2");
