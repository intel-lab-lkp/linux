/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */

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
	struct mutex rxe_sk_lock;
};

/*
 * Index to store custom data for each network namespace.
 */
static unsigned int rxe_pernet_id;

/*
 * Called for every existing and added network namespaces
 */
static int rxe_ns_init(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	/* defer socket create in the namespace to the first
	 * device create.
	 */
	mutex_init(&ns_sk->rxe_sk_lock);

	return 0;
}

static void rxe_ns_exit(struct net *net)
{
	/* called when the network namespace is removed
	 */
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);
	struct sock *sk;

	rcu_read_lock();
	sk = rcu_dereference(ns_sk->rxe_sk4);
	rcu_read_unlock();
	if (sk) {
		rcu_assign_pointer(ns_sk->rxe_sk4, NULL);
		udp_tunnel_sock_release(sk->sk_socket);
	}

#if IS_ENABLED(CONFIG_IPV6)
	rcu_read_lock();
	sk = rcu_dereference(ns_sk->rxe_sk6);
	rcu_read_unlock();
	if (sk) {
		rcu_assign_pointer(ns_sk->rxe_sk6, NULL);
		udp_tunnel_sock_release(sk->sk_socket);
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

void rxe_ns_pernet_sk_lock(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	mutex_lock(&ns_sk->rxe_sk_lock);
}

void rxe_ns_pernet_sk_unlock(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	mutex_unlock(&ns_sk->rxe_sk_lock);
}

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

#if IS_ENABLED(CONFIG_IPV6)
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
#endif /* IPV6 */

int rxe_namespace_init(void)
{
	return register_pernet_subsys(&rxe_net_ops);
}

void rxe_namespace_exit(void)
{
	unregister_pernet_subsys(&rxe_net_ops);
}
