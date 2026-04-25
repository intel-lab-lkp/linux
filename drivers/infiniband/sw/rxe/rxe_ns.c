/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */

#include <net/sock.h>
#include <net/netns/generic.h>
#include <net/net_namespace.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/pid_namespace.h>
#include <net/udp_tunnel.h>
#include <rdma/ib_verbs.h>

#include "rxe_ns.h"
#include "rxe_net.h"

/*
 * Per network namespace data
 */
struct rxe_ns_sock {
	struct sock __rcu *rxe_sk4;
	struct sock __rcu *rxe_sk6;
};

/*
 * Index to store custom data for each network namespace.
 */
static unsigned int rxe_pernet_id;

static __net_init int rxe_ns_init(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);
	struct sock *sk;
	int err = 0;

	sk = rxe_setup_udp_tunnel(net, htons(ROCE_V2_UDP_DPORT), false);
	if (IS_ERR(sk)) {
		err = PTR_ERR(sk);
		goto out;
	}

	RCU_INIT_POINTER(ns_sk->rxe_sk4, sk);

#if IS_ENABLED(CONFIG_IPV6)
	sk = rxe_setup_udp_tunnel(net, htons(ROCE_V2_UDP_DPORT), true);
	if (IS_ERR(sk)) {
		err = PTR_ERR(sk);
		if (err == -EAFNOSUPPORT) {
			err = 0;
			goto out;
		}

		sk = rcu_dereference_protected(ns_sk->rxe_sk4, 1);
		rxe_release_udp_tunnel(sk);
		goto out;
	}

	RCU_INIT_POINTER(ns_sk->rxe_sk6, sk);
#endif
out:
	return err;
}

static __net_exit void rxe_ns_exit(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);
	struct sock *sk;

	sk = rcu_dereference_protected(ns_sk->rxe_sk4, 1);
	RCU_INIT_POINTER(ns_sk->rxe_sk4, NULL);
	rxe_release_udp_tunnel(sk);

#if IS_ENABLED(CONFIG_IPV6)
	sk = rcu_dereference_protected(ns_sk->rxe_sk6, 1);
	if (sk) {
		RCU_INIT_POINTER(ns_sk->rxe_sk6, NULL);
		rxe_release_udp_tunnel(sk);
	}
#endif
}

/*
 * callback to make the module network namespace aware
 */
static struct pernet_operations rxe_net_ops = {
	.init = rxe_ns_init,
	.exit = rxe_ns_exit,
	.id = &rxe_pernet_id,
	.size = sizeof(struct rxe_ns_sock),
};

#if IS_ENABLED(CONFIG_IPV6)
struct sock *rxe_ns_pernet_sk6(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	return rcu_dereference(ns_sk->rxe_sk6);
}
#endif /* IPV6 */

int rxe_namespace_init(void)
{
	return register_pernet_subsys(&rxe_net_ops);
}

void rxe_namespace_exit(void)
{
	unregister_pernet_subsys(&rxe_net_ops);
}
