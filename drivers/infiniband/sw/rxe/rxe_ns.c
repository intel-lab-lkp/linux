/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */

#include <net/sock.h>
#include <net/netns/generic.h>
#include <net/net_namespace.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/skbuff.h>
#include <linux/pid_namespace.h>
#include <net/udp_tunnel.h>

#include "rxe_ns.h"

/*
 * Per network namespace data.
 *
 * The IPv4/IPv6 UDP tunnel sockets are shared by every rxe device created in
 * a given namespace. Their lifetime is owned here and tracked by an explicit
 * user count (nr_skX) rather than by overloading sk->sk_refcnt: the network
 * stack takes transient references on sk_refcnt of its own, so it can never be
 * used to decide when the last rxe device is gone. All fields are serialised
 * by @lock.
 */
struct rxe_ns_sock {
	struct mutex lock; /* protects rxe_sk4/6 and nr_sk4/6 */
	struct sock __rcu *rxe_sk4;
	struct sock __rcu *rxe_sk6;
	int nr_sk4;
	int nr_sk6;
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

	/* Socket creation is deferred to the first device create. */
	mutex_init(&ns_sk->lock);

	return 0;
}

static void rxe_ns_exit(struct net *net)
{
	/* called when the network namespace is removed */
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);
	struct sock *sk4, *sk6;

	mutex_lock(&ns_sk->lock);
	sk4 = rcu_dereference_protected(ns_sk->rxe_sk4,
					lockdep_is_held(&ns_sk->lock));
	sk6 = rcu_dereference_protected(ns_sk->rxe_sk6,
					lockdep_is_held(&ns_sk->lock));
	rcu_assign_pointer(ns_sk->rxe_sk4, NULL);
	rcu_assign_pointer(ns_sk->rxe_sk6, NULL);
	ns_sk->nr_sk4 = 0;
	ns_sk->nr_sk6 = 0;
	mutex_unlock(&ns_sk->lock);

	if (sk4 || sk6)
		synchronize_rcu();
	if (sk4)
		udp_tunnel_sock_release(sk4);
	if (sk6)
		udp_tunnel_sock_release(sk6);

	mutex_destroy(&ns_sk->lock);
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

static struct sock *rxe_ns_hold(struct rxe_ns_sock *ns_sk,
				struct sock __rcu **skp, int *nrp,
				struct net *net, rxe_sk_create_t create)
{
	struct sock *sk;

	mutex_lock(&ns_sk->lock);
	sk = rcu_dereference_protected(*skp, lockdep_is_held(&ns_sk->lock));
	if (sk) {
		(*nrp)++;
		mutex_unlock(&ns_sk->lock);
		return sk;
	}

	sk = create(net);
	if (IS_ERR_OR_NULL(sk)) {
		mutex_unlock(&ns_sk->lock);
		return sk;
	}

	rcu_assign_pointer(*skp, sk);
	*nrp = 1;
	mutex_unlock(&ns_sk->lock);

	return sk;
}

static void rxe_ns_put(struct rxe_ns_sock *ns_sk,
		       struct sock __rcu **skp, int *nrp)
{
	struct sock *sk = NULL;

	mutex_lock(&ns_sk->lock);
	if (*nrp > 0 && --(*nrp) == 0) {
		sk = rcu_dereference_protected(*skp,
					       lockdep_is_held(&ns_sk->lock));
		rcu_assign_pointer(*skp, NULL);
	}
	mutex_unlock(&ns_sk->lock);

	if (sk) {
		synchronize_rcu();
		udp_tunnel_sock_release(sk);
	}
}

struct sock *rxe_ns_pernet_hold_sk4(struct net *net, rxe_sk_create_t create)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	return rxe_ns_hold(ns_sk, &ns_sk->rxe_sk4, &ns_sk->nr_sk4, net, create);
}

void rxe_ns_pernet_put_sk4(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	rxe_ns_put(ns_sk, &ns_sk->rxe_sk4, &ns_sk->nr_sk4);
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

struct sock *rxe_ns_pernet_hold_sk6(struct net *net, rxe_sk_create_t create)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	return rxe_ns_hold(ns_sk, &ns_sk->rxe_sk6, &ns_sk->nr_sk6, net, create);
}

void rxe_ns_pernet_put_sk6(struct net *net)
{
	struct rxe_ns_sock *ns_sk = net_generic(net, rxe_pernet_id);

	rxe_ns_put(ns_sk, &ns_sk->rxe_sk6, &ns_sk->nr_sk6);
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
