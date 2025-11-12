
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/socket.h>
#include <linux/udp.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/in6.h>
#include <net/udp.h>
#include <net/udp_tunnel.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/ip6_tunnel.h>
#include <net/ip6_checksum.h>

int udp_sock_create6(struct net *net, struct udp_port_cfg *cfg,
		     struct socket **sockp)
{
	struct sockaddr_in6 udp6_addr = {};
	int err;
	struct socket *sock = NULL;

	err = sock_create_kern(net, AF_INET6, SOCK_DGRAM, 0, &sock);
	if (err < 0)
		goto error;

	if (cfg->ipv6_v6only) {
		err = ip6_sock_set_v6only(sock->sk);
		if (err < 0)
			goto error;
	}
	if (cfg->bind_ifindex) {
		err = sock_bindtoindex(sock->sk, cfg->bind_ifindex, true);
		if (err < 0)
			goto error;
	}

	udp6_addr.sin6_family = AF_INET6;
	memcpy(&udp6_addr.sin6_addr, &cfg->local_ip6,
	       sizeof(udp6_addr.sin6_addr));
	udp6_addr.sin6_port = cfg->local_udp_port;
	err = kernel_bind(sock, (struct sockaddr_unsized *)&udp6_addr,
			  sizeof(udp6_addr));
	if (err < 0)
		goto error;

	if (cfg->peer_udp_port) {
		memset(&udp6_addr, 0, sizeof(udp6_addr));
		udp6_addr.sin6_family = AF_INET6;
		memcpy(&udp6_addr.sin6_addr, &cfg->peer_ip6,
		       sizeof(udp6_addr.sin6_addr));
		udp6_addr.sin6_port = cfg->peer_udp_port;
		err = kernel_connect(sock,
				     (struct sockaddr_unsized *)&udp6_addr,
				     sizeof(udp6_addr), 0);
	}
	if (err < 0)
		goto error;

	udp_set_no_check6_tx(sock->sk, !cfg->use_udp6_tx_checksums);
	udp_set_no_check6_rx(sock->sk, !cfg->use_udp6_rx_checksums);

	*sockp = sock;
	return 0;

error:
	if (sock) {
		kernel_sock_shutdown(sock, SHUT_RDWR);
		sock_release(sock);
	}
	*sockp = NULL;
	return err;
}
EXPORT_SYMBOL_GPL(udp_sock_create6);

void udp_tunnel6_xmit_skb(dstref_t dstref, struct sock *sk,
			  struct sk_buff *skb,
			  struct net_device *dev,
			  const struct in6_addr *saddr,
			  const struct in6_addr *daddr,
			  __u8 prio, __u8 ttl, __be32 label,
			  __be16 src_port, __be16 dst_port, bool nocheck,
			  u16 ip6cb_flags)
{
	struct udphdr *uh;
	struct ipv6hdr *ip6h;

	__skb_push(skb, sizeof(*uh));
	skb_reset_transport_header(skb);
	uh = udp_hdr(skb);

	uh->dest = dst_port;
	uh->source = src_port;

	uh->len = htons(skb->len);

	skb_dstref_set(skb, dstref);

	udp6_set_csum(nocheck, skb, saddr, daddr, skb->len);

	__skb_push(skb, sizeof(*ip6h));
	skb_reset_network_header(skb);
	ip6h		  = ipv6_hdr(skb);
	ip6_flow_hdr(ip6h, prio, label);
	ip6h->payload_len = htons(skb->len);
	ip6h->nexthdr     = IPPROTO_UDP;
	ip6h->hop_limit   = ttl;
	ip6h->daddr	  = *daddr;
	ip6h->saddr	  = *saddr;

	ip6tunnel_xmit(sk, skb, dev, ip6cb_flags);
}
EXPORT_SYMBOL_GPL(udp_tunnel6_xmit_skb);

/**
 *      udp_tunnel6_dst_lookup - perform route lookup on UDP tunnel
 *      @skb: Packet for which lookup is done
 *      @dev: Tunnel device
 *      @net: Network namespace of tunnel device
 *      @sock: Socket which provides route info
 *      @oif: Index of the output interface
 *      @saddr: Memory to store the src ip address
 *      @key: Tunnel information
 *      @sport: UDP source port
 *      @dport: UDP destination port
 *      @dsfield: The traffic class field
 *      @dst_cache: The dst cache to use for lookup
 *      @dstref: Memory to store the dstref object returned from the lookup
 *      This function performs a route lookup on a UDP tunnel
 *
 *      On success, it stores the dstref object that represents the result of the lookup
 *      in the dstref param, and the src address to be used for the tunnel in the saddr param.
 *
 *      Returns: 0 on success, negative error code on failure
 */

int udp_tunnel6_dst_lookup(struct sk_buff *skb,
			   struct net_device *dev,
			   struct net *net,
			   struct socket *sock, int oif,
			   struct in6_addr *saddr,
			   const struct ip_tunnel_key *key,
			   __be16 sport, __be16 dport, u8 dsfield,
			   struct dst_cache *dst_cache, dstref_t *dstref)
{
	struct dst_entry *dst = NULL;
	struct flowi6 fl6;

#ifdef CONFIG_DST_CACHE
	if (dst_cache) {
		dst = dst_cache_get_ip6_rcu(dst_cache, saddr);
		if (dst) {
			*dstref = dst_to_dstref_noref(dst);
			return 0;
		}
	}
#endif
	memset(&fl6, 0, sizeof(fl6));
	fl6.flowi6_mark = skb->mark;
	fl6.flowi6_proto = IPPROTO_UDP;
	fl6.flowi6_oif = oif;
	fl6.daddr = key->u.ipv6.dst;
	fl6.saddr = key->u.ipv6.src;
	fl6.fl6_sport = sport;
	fl6.fl6_dport = dport;
	fl6.flowlabel = ip6_make_flowinfo(dsfield, key->label);

	dst = ipv6_stub->ipv6_dst_lookup_flow(net, sock->sk, &fl6,
					      NULL);
	if (IS_ERR(dst)) {
		netdev_dbg(dev, "no route to %pI6\n", &fl6.daddr);
		return -ENETUNREACH;
	}
	if (dst_dev(dst) == dev) { /* is this necessary? */
		netdev_dbg(dev, "circular route to %pI6\n", &fl6.daddr);
		dst_release(dst);
		return -ELOOP;
	}
	*saddr = fl6.saddr;
#ifdef CONFIG_DST_CACHE
	if (dst_cache) {
		dst_cache_steal_ip6(dst_cache, dst, &fl6.saddr);
		*dstref = dst_to_dstref_noref(dst);
		return 0;
	}
#endif
	*dstref = dst_to_dstref(dst);
	return 0;
}
EXPORT_SYMBOL_GPL(udp_tunnel6_dst_lookup);

MODULE_DESCRIPTION("IPv6 Foo over UDP tunnel driver");
MODULE_LICENSE("GPL");
