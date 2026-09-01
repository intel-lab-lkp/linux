// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * udp_diag.c	Module for monitoring UDP transport protocols sockets.
 *
 * Authors:	Pavel Emelyanov, <xemul@parallels.com>
 */


#include <linux/module.h>
#include <linux/inet_diag.h>
#include <linux/udp.h>
#include <net/udp.h>
#include <linux/sock_diag.h>

static int sk_diag_dump(struct sock *sk, struct sk_buff *skb,
			struct netlink_callback *cb,
			const struct inet_diag_req_v2 *req,
			bool net_admin)
{
	if (!inet_diag_bc_sk(cb->data, sk))
		return 0;

	return inet_sk_diag_fill(sk, NULL, skb, cb, req, NLM_F_MULTI,
				 net_admin);
}

/* Process a maximum of SKARR_SZ hash entries at a time when walking hash
 * buckets with bh disabled.
 */
#define SKARR_SZ 16

static bool udp_diag_cursor_valid(struct udp_table *table,
				  struct udp_hslot *hslot,
				  struct sock *sk)
{
	if (!sk || hlist_unhashed_lockless(&sk->sk_node))
		return false;

	return sock_net(sk)->ipv4.udp_table == table &&
	       udp_hashslot(table, sock_net(sk),
			    udp_sk(sk)->udp_port_hash) == hslot;
}

static void udp_diag_dump_done(struct netlink_callback *cb)
{
	struct inet_diag_dump_data *cb_data = cb->data;
	struct sock *sk = (struct sock *)cb->args[2];

	if (sk) {
		cb->args[2] = 0;
		sock_put(sk);
	}
	cb_data->dump_done = NULL;
	module_put(THIS_MODULE);
}

static int udp_diag_dump_one(struct netlink_callback *cb,
			     const struct inet_diag_req_v2 *req)
{
	struct sk_buff *in_skb = cb->skb;
	struct sock *sk = NULL;
	struct sk_buff *rep;
	struct net *net;
	int err;

	net = sock_net(in_skb->sk);

	rcu_read_lock();
	if (req->sdiag_family == AF_INET)
		/* src and dst are swapped for historical reasons */
		sk = __udp4_lib_lookup(net,
				       req->id.idiag_src[0], req->id.idiag_sport,
				       req->id.idiag_dst[0], req->id.idiag_dport,
				       req->id.idiag_if, 0, NULL);
#if IS_ENABLED(CONFIG_IPV6)
	else if (req->sdiag_family == AF_INET6)
		sk = __udp6_lib_lookup(net,
				       (struct in6_addr *)req->id.idiag_src,
				       req->id.idiag_sport,
				       (struct in6_addr *)req->id.idiag_dst,
				       req->id.idiag_dport,
				       req->id.idiag_if, 0, NULL);
#endif
	if (sk && !refcount_inc_not_zero(&sk->sk_refcnt))
		sk = NULL;
	rcu_read_unlock();
	err = -ENOENT;
	if (!sk)
		goto out_nosk;

	err = sock_diag_check_cookie(sk, req->id.idiag_cookie);
	if (err)
		goto out;

	err = -ENOMEM;
	rep = nlmsg_new(nla_total_size(sizeof(struct inet_diag_msg)) +
			inet_diag_msg_attrs_size() +
			nla_total_size(sizeof(struct inet_diag_meminfo)) + 64,
			GFP_KERNEL);
	if (!rep)
		goto out;

	err = inet_sk_diag_fill(sk, NULL, rep, cb, req, 0,
				netlink_net_capable(in_skb, CAP_NET_ADMIN));
	if (err < 0) {
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(rep);
		goto out;
	}
	err = nlmsg_unicast(net->diag_nlsk, rep, NETLINK_CB(in_skb).portid);

out:
	if (sk)
		sock_put(sk);
out_nosk:
	return err;
}

static void udp_diag_dump(struct sk_buff *skb, struct netlink_callback *cb,
			  const struct inet_diag_req_v2 *r)
{
	bool net_admin = netlink_net_capable(cb->skb, CAP_NET_ADMIN);
	struct sock *cursor = (struct sock *)cb->args[2];
	struct inet_diag_dump_data *cb_data = cb->data;
	struct net *net = sock_net(skb->sk);
	unsigned int slot = cb->args[0];
	struct udp_table *table;

	table = net->ipv4.udp_table;
	/* Keep this module loaded until dump_done() drops the cursor. */
	if (!cb_data->dump_done) {
		__module_get(THIS_MODULE);
		cb_data->dump_done = udp_diag_dump_done;
	}

	for (; slot <= table->mask; slot++) {
		struct udp_hslot *hslot = &table->hash[slot];

		for (;;) {
			struct sock *sk, *next_cursor = NULL;
			int idx, accum = 0, walked = 0, res;
			struct sock *old_cursor = NULL;
			struct sock *sk_arr[SKARR_SZ];

			spin_lock_bh(&hslot->lock);
			sk = cursor;
			if (sk && !udp_diag_cursor_valid(table, hslot, sk)) {
				old_cursor = sk;
				cursor = NULL;
				sk = NULL;
			}
			if (!sk)
				sk = hlist_entry_safe(hslot->head.first,
						      struct sock, sk_node);

			while (sk && walked < SKARR_SZ) {
				struct inet_sock *inet = inet_sk(sk);
				struct sock *next;

				next = hlist_entry_safe(sk->sk_node.next,
							struct sock, sk_node);
				if (net_eq(sock_net(sk), net) &&
				    (r->idiag_states & (1 << sk->sk_state)) &&
				    (r->sdiag_family == AF_UNSPEC ||
				     sk->sk_family == r->sdiag_family) &&
				    (r->id.idiag_sport == inet->inet_sport ||
				     !r->id.idiag_sport) &&
				    (r->id.idiag_dport == inet->inet_dport ||
				     !r->id.idiag_dport)) {
					sock_hold(sk);
					sk_arr[accum++] = sk;
				}

				walked++;
				if (walked == SKARR_SZ) {
					if (next) {
						sock_hold(next);
						next_cursor = next;
					}
					break;
				}
				sk = next;
			}
			spin_unlock_bh(&hslot->lock);
			/* Consume the resume ref; remaining refs are in
			 * sk_arr / next_cursor.
			 */
			if (old_cursor)
				sock_put(old_cursor);
			else if (cursor)
				sock_put(cursor);
			cursor = NULL;

			for (idx = 0; idx < accum; idx++) {
				res = sk_diag_dump(sk_arr[idx], skb, cb, r,
						   net_admin);
				if (res < 0) {
					cursor = sk_arr[idx];
					while (++idx < accum)
						sock_put(sk_arr[idx]);
					if (next_cursor)
						sock_put(next_cursor);
					goto done;
				}
				sock_put(sk_arr[idx]);
			}

			cursor = next_cursor;
			if (!cursor)
				break;

			cond_resched();
		}
	}

done:
	cb->args[0] = slot;
	cb->args[1] = 0;
	cb->args[2] = (unsigned long)cursor;
}

static void udp_diag_get_info(struct sock *sk, struct inet_diag_msg *r,
		void *info)
{
	r->idiag_rqueue = udp_rqueue_get(sk);
	r->idiag_wqueue = sk_wmem_alloc_get(sk);
}

#ifdef CONFIG_INET_DIAG_DESTROY
static int udp_diag_destroy(struct sk_buff *in_skb,
			    const struct inet_diag_req_v2 *req)
{
	struct net *net = sock_net(in_skb->sk);
	struct sock *sk;
	int err;

	rcu_read_lock();

	if (req->sdiag_family == AF_INET)
		sk = __udp4_lib_lookup(net,
				       req->id.idiag_dst[0], req->id.idiag_dport,
				       req->id.idiag_src[0], req->id.idiag_sport,
				       req->id.idiag_if, 0, NULL);
#if IS_ENABLED(CONFIG_IPV6)
	else if (req->sdiag_family == AF_INET6) {
		if (ipv6_addr_v4mapped((struct in6_addr *)req->id.idiag_dst) &&
		    ipv6_addr_v4mapped((struct in6_addr *)req->id.idiag_src))
			sk = __udp4_lib_lookup(net,
					       req->id.idiag_dst[3], req->id.idiag_dport,
					       req->id.idiag_src[3], req->id.idiag_sport,
					       req->id.idiag_if, 0, NULL);
		else
			sk = __udp6_lib_lookup(net,
					       (struct in6_addr *)req->id.idiag_dst,
					       req->id.idiag_dport,
					       (struct in6_addr *)req->id.idiag_src,
					       req->id.idiag_sport,
					       req->id.idiag_if, 0, NULL);
	}
#endif
	else {
		rcu_read_unlock();
		return -EINVAL;
	}

	if (sk && !refcount_inc_not_zero(&sk->sk_refcnt))
		sk = NULL;

	rcu_read_unlock();

	if (!sk)
		return -ENOENT;

	if (sock_diag_check_cookie(sk, req->id.idiag_cookie)) {
		sock_put(sk);
		return -ENOENT;
	}

	err = sock_diag_destroy(sk, ECONNABORTED);

	sock_put(sk);

	return err;
}
#endif

static const struct inet_diag_handler udp_diag_handler = {
	.owner		 = THIS_MODULE,
	.dump		 = udp_diag_dump,
	.dump_one	 = udp_diag_dump_one,
	.idiag_get_info  = udp_diag_get_info,
	.idiag_type	 = IPPROTO_UDP,
	.idiag_info_size = 0,
#ifdef CONFIG_INET_DIAG_DESTROY
	.destroy	 = udp_diag_destroy,
#endif
};

static int __init udp_diag_init(void)
{
	return inet_diag_register(&udp_diag_handler);
}

static void __exit udp_diag_exit(void)
{
	inet_diag_unregister(&udp_diag_handler);
}

module_init(udp_diag_init);
module_exit(udp_diag_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UDP socket monitoring via SOCK_DIAG");
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_NETLINK, NETLINK_SOCK_DIAG, 2-17 /* AF_INET - IPPROTO_UDP */);
