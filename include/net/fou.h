/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_FOU_H
#define __NET_FOU_H

#include <linux/skbuff.h>

#include <net/flow.h>
#include <net/gue.h>
#include <net/ip_tunnels.h>
#include <net/udp.h>

size_t fou_encap_hlen(struct ip_tunnel_encap *e);
size_t gue_encap_hlen(struct ip_tunnel_encap *e);

int __fou_build_header(struct sk_buff *skb, struct ip_tunnel_encap *e,
		       u8 *protocol, __be16 *sport, int type);
int __gue_build_header(struct sk_buff *skb, struct ip_tunnel_encap *e,
		       u8 *protocol, __be16 *sport, int type);

struct fou {
	struct socket *sock;
	u8 protocol;
	u8 flags;
	__be16 port;
	u8 family;
	u16 type;
	struct list_head list;
	struct rcu_head rcu;
};

static inline struct fou *fou_from_sock(struct sock *sk)
{
	return sk->sk_user_data;
}

int register_fou_bpf(void);

#endif
