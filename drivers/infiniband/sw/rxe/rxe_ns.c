// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include <net/sock.h>
#include <net/netns/generic.h>
#include <net/net_namespace.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/pid_namespace.h>
#include <net/udp_tunnel.h>

#include "rxe_ns.h"

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

/*
 * Called for every existing and added network namespaces
 */
static int __net_init rxe_ns_init(struct net *net)
{
	/*
	 * create (if not present) and access data item in network namespace
	 * (net) using the id (net_id)
	 */
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	rcu_assign_pointer(ns_sk->rxe_sk4, NULL); /* initialize sock 4 socket */
	rcu_assign_pointer(ns_sk->rxe_sk6, NULL); /* initialize sock 6 socket */
	synchronize_rcu();

	return 0;
}

static void __net_exit rxe_ns_exit(struct net *net)
{
	/*
	 * called when the network namespace is removed
	 */
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);
	struct sock *rxe_sk4 = NULL;
	struct sock *rxe_sk6 = NULL;

	rcu_read_lock();
	rxe_sk4 = rcu_dereference(ns_sk->rxe_sk4);
	rxe_sk6 = rcu_dereference(ns_sk->rxe_sk6);
	rcu_read_unlock();

	/* close socket */
	if (rxe_sk4 && rxe_sk4->sk_socket) {
		udp_tunnel_sock_release(rxe_sk4->sk_socket);
		rcu_assign_pointer(ns_sk->rxe_sk4, NULL);
		synchronize_rcu();
	}

	if (rxe_sk6 && rxe_sk6->sk_socket) {
		udp_tunnel_sock_release(rxe_sk6->sk_socket);
		rcu_assign_pointer(ns_sk->rxe_sk6, NULL);
		synchronize_rcu();
	}
}

/*
 * callback to make the module network namespace aware
 */
static struct pernet_operations rxe_net_ops __net_initdata = {
	.init = rxe_ns_init,
	.exit = rxe_ns_exit,
	.id = &rxe_pernet_id,
	.size = sizeof(struct rxe_ns_sock),
};

struct sock *rxe_ns_pernet_sk4(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);
	struct sock *sk;

	rcu_read_lock();
	sk = rcu_dereference(ns_sk->rxe_sk4);
	rcu_read_unlock();

	return sk;
}

void rxe_ns_pernet_set_sk4(struct net *net, struct sock *sk)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	rcu_assign_pointer(ns_sk->rxe_sk4, sk);
	synchronize_rcu();
}

struct sock *rxe_ns_pernet_sk6(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);
	struct sock *sk;

	rcu_read_lock();
	sk = rcu_dereference(ns_sk->rxe_sk6);
	rcu_read_unlock();

	return sk;
}

void rxe_ns_pernet_set_sk6(struct net *net, struct sock *sk)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	rcu_assign_pointer(ns_sk->rxe_sk6, sk);
	synchronize_rcu();
}

int __init rxe_namespace_init(void)
{
	return register_pernet_subsys(&rxe_net_ops);
}

void __exit rxe_namespace_exit(void)
{
	unregister_pernet_subsys(&rxe_net_ops);
}
