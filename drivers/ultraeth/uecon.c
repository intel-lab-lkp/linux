// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/etherdevice.h>
#include <linux/if_link.h>
#include <net/ipv6_stubs.h>
#include <net/dst_metadata.h>
#include <net/rtnetlink.h>
#include <net/gro.h>
#include <net/udp_tunnel.h>
#include <net/ultraeth/uecon.h>
#include <net/ultraeth/uet_context.h>

static const struct nla_policy uecon_ndev_policy[IFLA_UECON_MAX + 1] = {
	[IFLA_UECON_CONTEXT_ID]	= { .type = NLA_REJECT,
				    .reject_message = "Domain id attribute is read-only" },
	[IFLA_UECON_PORT]	= { .type = NLA_BE16 },
};

static netdev_tx_t uecon_ndev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct uecon_ndev_priv *uecpriv = netdev_priv(dev);
	struct ip_tunnel_info *info;
	int err, min_headroom;
	struct socket *sock;
	struct rtable *rt;
	bool use_cache;
	__be32 saddr;
	__be16 sport;

	rcu_read_lock();
	sock = rcu_dereference(uecpriv->sock);
	if (!sock)
		goto out_err;
	info = skb_tunnel_info(skb);
	if (!info)
		goto out_err;
	use_cache = ip_tunnel_dst_cache_usable(skb, info);
	sport = uecpriv->udp_port;
	rt = udp_tunnel_dst_lookup(skb, dev, dev_net(dev), 0, &saddr,
				   &info->key, sport,
				   info->key.tp_dst, info->key.tos,
				   use_cache ? &info->dst_cache : NULL);
	if (IS_ERR(rt)) {
		if (PTR_ERR(rt) == -ELOOP)
			dev->stats.collisions++;
		else if (PTR_ERR(rt) == -ENETUNREACH)
			dev->stats.tx_carrier_errors++;

		goto out_err;
	}

	skb_tunnel_check_pmtu(skb, &rt->dst,
			      sizeof(struct iphdr) + sizeof(struct udphdr),
			      false);
	skb_scrub_packet(skb, false);

	min_headroom = LL_RESERVED_SPACE(rt->dst.dev) + rt->dst.header_len +
		       sizeof(struct iphdr) + sizeof(struct udphdr);
	err = skb_cow_head(skb, min_headroom);
	if (unlikely(err)) {
		dst_release(&rt->dst);
		goto out_err;
	}

	err = udp_tunnel_handle_offloads(skb, false);
	if (err) {
		dst_release(&rt->dst);
		goto out_err;
	}

	skb_reset_mac_header(skb);
	skb_set_inner_protocol(skb, skb->protocol);

	udp_tunnel_xmit_skb(rt, sock->sk, skb, saddr,
			    info->key.u.ipv4.dst, info->key.tos,
			    ip4_dst_hoplimit(&rt->dst), 0,
			    sport, info->key.tp_dst,
			    false, false);
	rcu_read_unlock();

	return NETDEV_TX_OK;

out_err:
	rcu_read_unlock();
	dev_kfree_skb(skb);
	dev->stats.tx_errors++;

	return NETDEV_TX_OK;
}

static int uecon_ndev_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct uecon_ndev_priv *uecpriv;
	int len;

	uecpriv = rcu_dereference_sk_user_data(sk);
	if (!uecpriv)
		goto drop;

	if (skb->protocol != htons(ETH_P_IP))
		goto drop;

	/* we assume [ tnl ip hdr ] [ tnl udp hdr ] [ pdc hdr ] [ ses hdr ] */
	if (iptunnel_pull_header(skb, sizeof(struct udphdr), htons(ETH_P_802_3), false))
		goto drop_count;

	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb->pkt_type = PACKET_HOST;
	skb->dev = uecpriv->dev;
	len = skb->len;
	consume_skb(skb);
	dev_sw_netstats_rx_add(uecpriv->dev, len);

	return 0;

drop_count:
	dev_core_stats_rx_dropped_inc(uecpriv->dev);
drop:
	kfree_skb(skb);
	return 0;
}

static int uecon_ndev_err_lookup(struct sock *sk, struct sk_buff *skb)
{
	return 0;
}

static struct socket *uecon_ndev_create_sock(struct net_device *dev)
{
	struct uecon_ndev_priv *uecpriv = netdev_priv(dev);
	struct udp_port_cfg udp_conf;
	struct socket *sock;
	int err;

	memset(&udp_conf, 0, sizeof(udp_conf));
	udp_conf.family = AF_INET;
	udp_conf.local_udp_port = uecpriv->udp_port;
	err = udp_sock_create(dev_net(dev), &udp_conf, &sock);
	if (err < 0)
		return ERR_PTR(err);

	udp_allow_gso(sock->sk);

	return sock;
}

static int uecon_ndev_open(struct net_device *dev)
{
	struct uecon_ndev_priv *uecpriv = netdev_priv(dev);
	struct udp_tunnel_sock_cfg tunnel_cfg;
	struct socket *sock;

	sock = uecon_ndev_create_sock(dev);
	if (IS_ERR(sock))
		return PTR_ERR(sock);
	memset(&tunnel_cfg, 0, sizeof(tunnel_cfg));
	tunnel_cfg.sk_user_data = uecpriv;
	tunnel_cfg.encap_type = 1;
	tunnel_cfg.encap_rcv = uecon_ndev_encap_recv;
	tunnel_cfg.encap_err_lookup = uecon_ndev_err_lookup;
	setup_udp_tunnel_sock(dev_net(dev), sock, &tunnel_cfg);

	rcu_assign_pointer(uecpriv->sock, sock);

	return 0;
}

static int uecon_ndev_stop(struct net_device *dev)
{
	struct uecon_ndev_priv *uecpriv = netdev_priv(dev);
	struct socket *sock = rtnl_dereference(uecpriv->sock);

	rcu_assign_pointer(uecpriv->sock, NULL);
	synchronize_rcu();
	udp_tunnel_sock_release(sock);

	return 0;
}

const struct net_device_ops uecon_netdev_ops = {
	.ndo_open		= uecon_ndev_open,
	.ndo_stop		= uecon_ndev_stop,
	.ndo_start_xmit		= uecon_ndev_xmit,
	.ndo_get_stats64	= dev_get_tstats64,
};

static const struct device_type uecon_ndev_type = {
	.name = "uecon",
};

static void uecon_ndev_setup(struct net_device *dev)
{
	struct uecon_ndev_priv *uecpriv = netdev_priv(dev);

	dev->netdev_ops = &uecon_netdev_ops;
	SET_NETDEV_DEVTYPE(dev, &uecon_ndev_type);

	dev->features |= NETIF_F_VLAN_CHALLENGED | NETIF_F_SG | NETIF_F_HW_CSUM;
	dev->features |= NETIF_F_FRAGLIST | NETIF_F_RXCSUM | NETIF_F_GSO_SOFTWARE;

	dev->hw_features |= NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_FRAGLIST;
	dev->hw_features |= NETIF_F_RXCSUM | NETIF_F_GSO_SOFTWARE;

	dev->priv_flags |= IFF_NO_QUEUE;

	dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	dev->type = ARPHRD_NONE;

	dev->min_mtu = IPV4_MIN_MTU;
	/* No header for the time being, account for it later */
	dev->max_mtu = IP_MAX_MTU;
	dev->mtu = ETH_DATA_LEN;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;

	netif_keep_dst(dev);
	uecpriv->dev = dev;
}

static int uecon_ndev_changelink(struct net_device *dev, struct nlattr *tb[],
				 struct nlattr *data[],
				 struct netlink_ext_ack *extack)
{
	if (dev->flags & IFF_UP) {
		NL_SET_ERR_MSG_MOD(extack, "Cannot change uecon settings while the device is up");
		return -EBUSY;
	}

	if (tb[IFLA_UECON_PORT]) {
		struct uecon_ndev_priv *uecpriv = netdev_priv(dev);

		uecpriv->udp_port = nla_get_be16(tb[IFLA_UECON_PORT]);
	}

	return 0;
}

static size_t uecon_ndev_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(__u32))  + /* IFLA_UECON_CONTEXT_ID */
	       nla_total_size(sizeof(__be16)) + /* IFLA_UECON_PORT */
	       0;
}

static int uecon_ndev_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct uecon_ndev_priv *uecpriv = netdev_priv(dev);

	if (nla_put_u32(skb, IFLA_UECON_CONTEXT_ID, uecpriv->context->id) ||
	    nla_put_be16(skb, IFLA_UECON_PORT, uecpriv->udp_port))
		return -EMSGSIZE;

	return 0;
}

static struct rtnl_link_ops uecon_netdev_link_ops __read_mostly = {
	.kind		= "uecon",
	.priv_size	= sizeof(struct uecon_ndev_priv),
	.setup		= uecon_ndev_setup,
	.get_size	= uecon_ndev_get_size,
	.fill_info	= uecon_ndev_fill_info,
	.changelink	= uecon_ndev_changelink,
	.policy		= uecon_ndev_policy,
	.maxtype	= IFLA_UECON_MAX
};

int uecon_netdev_init(struct uet_context *ctx)
{
	struct net *net = current->nsproxy->net_ns;
	struct uecon_ndev_priv *priv;
	char ifname[IFNAMSIZ];
	int ret;

	snprintf(ifname, IFNAMSIZ, "uecon%d", ctx->id);
	ctx->netdev = alloc_netdev(sizeof(struct uecon_ndev_priv), ifname,
				   NET_NAME_PREDICTABLE, uecon_ndev_setup);
	if (!ctx->netdev)
		return -ENOMEM;
	priv = netdev_priv(ctx->netdev);

	priv->context = ctx;
	priv->dev = ctx->netdev;
	priv->udp_port = htons(UECON_DEFAULT_PORT);
	ctx->netdev->rtnl_link_ops = &uecon_netdev_link_ops;
	dev_net_set(ctx->netdev, net);

	ret = register_netdev(ctx->netdev);
	if (ret) {
		free_netdev(ctx->netdev);
		ctx->netdev = NULL;
	}

	return ret;
}

void uecon_netdev_uninit(struct uet_context *ctx)
{
	unregister_netdev(ctx->netdev);
	free_netdev(ctx->netdev);
	ctx->netdev = NULL;
}

int uecon_rtnl_link_register(void)
{
	return rtnl_link_register(&uecon_netdev_link_ops);
}

void uecon_rtnl_link_unregister(void)
{
	rtnl_link_unregister(&uecon_netdev_link_ops);
}
