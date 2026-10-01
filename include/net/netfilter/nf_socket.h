/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NF_SOCK_H_
#define _NF_SOCK_H_

#include <net/sock.h>

struct sock *nf_sk_lookup_slow_v4(struct net *net, const struct sk_buff *skb,
				  const struct net_device *indev);

struct sock *nf_sk_lookup_slow_v6(struct net *net, const struct sk_buff *skb,
				  const struct net_device *indev);

/**
 * nf_skb_sk - Return the inet socket associated with an skb
 * @skb: The skb to fetch sk from.
 * @net: The network namespace the socket must belong to.
 *
 * On the output path, skb->sk may be non-INET when the skb is passing
 * through an IP tunnel.
 *
 * Return:
 * *sk* if it is an INET socket that belongs to @net, %NULL otherwise.
 */
static inline struct sock *nf_skb_sk(const struct sk_buff *skb, const struct net *net)
{
	struct sock *sk = skb->sk;

	if (sk && net_eq(net, sock_net(sk)) && sk_is_inet(sk))
		return sk;

	return NULL;
}
#endif
