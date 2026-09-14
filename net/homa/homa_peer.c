// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0+

/* This file provides functions related to homa_peer, homa_route, and
 * homa_peertab objects.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_rpc.h"

#include <linux/xxhash.h>

/* Wrapper to allow xxhash to be used as rhashtable hash function. */
static u32 xxh32_hash(const void *data, u32 len, u32 seed)
{
	return xxh32(data, len, seed);
}

const struct rhashtable_params peer_ht_params = {
	.key_len     = sizeof(struct homa_peer_key),
	.key_offset  = offsetof(struct homa_peer, ht_key),
	.head_offset = offsetof(struct homa_peer, ht_linkage),
	.nelem_hint = 10000,
	.hashfn = xxh32_hash,
	.obj_cmpfn = homa_peer_compare
};

const struct rhashtable_params route_ht_params = {
	.key_len     = sizeof(struct homa_route_key),
	.key_offset  = offsetof(struct homa_route, key),
	.head_offset = offsetof(struct homa_route, ht_linkage),
	.nelem_hint = 10000,
	.hashfn = xxh32_hash
};

/**
 * homa_peer_alloc_peertab() - Allocate and initialize a homa_peertab.
 *
 * Return:    A pointer to the new homa_peertab, or ERR_PTR(-errno) if there
 *            was a problem.
 */
struct homa_peertab *homa_peer_alloc_peertab(void)
{
	struct homa_peertab *peertab;
	int err;

	peertab = kzalloc_obj(*peertab, GFP_KERNEL);
	if (!peertab)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&peertab->lock);
	err = rhashtable_init(&peertab->peer_ht, &peer_ht_params);
	if (err)
		goto error;
	err = rhashtable_init(&peertab->route_ht, &route_ht_params);
	if (err) {
		rhashtable_destroy(&peertab->peer_ht);
		goto error;
	}
	peertab->route_ht_valid = true;
	rhashtable_walk_enter(&peertab->route_ht, &peertab->route_ht_iter);
	peertab->gc_threshold = 5000;
	peertab->net_max = 10000;
	peertab->idle_secs_min = 10;
	peertab->idle_secs_max = 120;

	homa_peer_update_sysctl_deps(peertab);
	return peertab;

error:
	homa_peer_free_peertab(peertab);
	return ERR_PTR(err);
}

/**
 * homa_peer_free_net() - Garbage collect all of the peer information
 * associated with a particular network namespace.
 * @hnet:    Network namespace whose peers should be freed. There must not
 *           be any active sockets or RPCs for this namespace.
 */
void homa_peer_free_net(struct homa_net *hnet)
{
	struct homa_peertab *peertab = hnet->homa->peertab;
	struct rhashtable_iter iter;
	struct homa_route *route;

	spin_lock_bh(&peertab->lock);
	rhashtable_walk_enter(&peertab->route_ht, &iter);
	rhashtable_walk_start(&iter);
	while (1) {
		route = rhashtable_walk_next(&iter);
		if (!route)
			break;
		if (IS_ERR(route))
			continue;
		if (route->key.hnet != hnet)
			continue;
		if (rhashtable_remove_fast(&peertab->route_ht,
					   &route->ht_linkage,
					   route_ht_params) == 0) {
			homa_route_release(route);
			hnet->num_routes--;
			peertab->num_routes--;
		}
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	WARN(hnet->num_routes != 0, "%s ended up with hnet->num_routes %d",
	     __func__, hnet->num_routes);
	spin_unlock_bh(&peertab->lock);
}

/**
 * homa_route_delete_fn() - This function is invoked for each entry in
 * the route_ht hash table by the rhashtable code when the table is being
 * deleted. It frees its argument.
 * @object:     homa_peer to free.
 * @dummy:      Not used.
 */
void homa_route_delete_fn(void *object, void *dummy)
	__must_hold(((struct homa_route *)object)->key.hnet->homa->peertab->lock)
{
	struct homa_route *route = object;

	/* Must unlink from the peer here (peertab is going away so
	 * it won't be safe to do it later, if the reference count doesn't
	 * drop to zero here).
	 */
	homa_peer_unlink(route);
	homa_route_release(route);
}

/**
 * homa_peer_free_peertab() - Destructor for homa_peertabs.
 * @peertab:  The table to destroy. Caller must ensure that it will never
 *            be accessed again.
 */
void homa_peer_free_peertab(struct homa_peertab *peertab)
{
	if (peertab->route_ht_valid) {
		rhashtable_walk_exit(&peertab->route_ht_iter);
		rhashtable_free_and_destroy(&peertab->route_ht,
					    homa_route_delete_fn,
					    NULL);
		rcu_barrier();
		rhashtable_destroy(&peertab->peer_ht);
		peertab->route_ht_valid = false;
	}
	kfree(peertab);
}

/**
 * homa_peer_alloc() - Allocate and initialize a new homa_peer object.
 * @hsk:        Socket for which the peer will be used.
 * @addr:       Address of the desired host: IPv4 addresses are represented
 *              as IPv4-mapped IPv6 addresses.
 * Return:      The peer associated with @addr, or a negative errno if an
 *              error occurred. On a successful return the reference count
 *              will be incremented for the returned peer. Sets hsk->error_msg
 *              on errors.
 */
struct homa_peer *homa_peer_alloc(struct homa_sock *hsk,
				  const struct in6_addr *addr)
{
	struct homa_peer *peer;

	peer = kzalloc_obj(*peer, GFP_ATOMIC);
	if (!peer) {
		hsk->error_msg = "couldn't allocate memory for homa_peer";
		return (struct homa_peer *)ERR_PTR(-ENOMEM);
	}
	peer->ht_key.addr = *addr;
	peer->ht_key.hnet = hsk->hnet;
	refcount_set(&peer->refs, 1);
	spin_lock_init(&peer->lock);

	return peer;
}

/**
 * homa_peer_free() - Release any resources in a peer and free the homa_peer
 * struct.
 * @peer:   Peer to free. The @refs field must be zero (this also implies
 *          for example, that the peer is not on any lists and
 *          peer->grantable_rpcs is empty).
 */
void homa_peer_free(struct homa_peer *peer)
{
	kfree(peer);
}

/**
 * homa_peer_get() - Returns the peer associated with a given host; creates
 * a new homa_peer if one doesn't already exist.
 * @hsk:        Socket where the peer will be used.
 * @addr:       Address of the desired host: IPv4 addresses are represented
 *              as IPv4-mapped IPv6 addresses.
 *
 * Return:      The peer associated with @addr, or a negative errno if an
 *              error occurred. On a successful return the reference count
 *              will be incremented for the returned peer. The caller must
 *              eventually call homa_peer_release to release the reference.
 */
struct homa_peer *homa_peer_get(struct homa_sock *hsk,
				const struct in6_addr *addr)
	__must_hold(hsk->homa->peertab->lock)
{
	struct homa_peertab *peertab = hsk->homa->peertab;
	struct homa_peer_key key;
	struct homa_peer *peer;
	int status;

	key.addr = *addr;
	key.hnet = hsk->hnet;

	/* Fast path: use existing entry if it exists. */
	peer = rhashtable_lookup(&peertab->peer_ht, &key, peer_ht_params);
	if (peer) {
		refcount_inc(&peer->refs);
		return peer;
	}

	/* No existing entry, so we have to create a new one. */
	peer = homa_peer_alloc(hsk, addr);
	if (IS_ERR(peer))
		return peer;
	status = rhashtable_lookup_insert_fast(&peertab->peer_ht,
					       &peer->ht_linkage,
					       peer_ht_params);
	if (status != 0) {
		pr_err("homa_peer_get received unexpected error return from rhashtable_lookup_insert_fast");
		hsk->error_msg = "unexpected error return from rhashtable_lookup_insert_fast";
		homa_peer_free(peer);
		return ERR_PTR(status);
	}
	return peer;
}

/**
 * homa_route_alloc() - Allocate and initialize a new homa_route.
 * object.
 * @hsk:       Socket for which the homa_route will be used.
 * @key:       Key that will be used for this entry in peertab->route_ht.
 *             Contains the destination address and other information needed
 *             to create a flowi and a dst_entry.
 * Return:     A new homa_route corresdponding to @hsk and @key, or a
 *             negative errno if an error occurred. Sets hsk->error_msg on
 *             errors. On success, all fields have been initialized except
 *             @peer; the route has not been inserted in route_ht and its
 *             reference count will be one.
 */
struct homa_route *homa_route_alloc(struct homa_sock *hsk,
				    const struct homa_route_key *key)
{
	struct homa_route *route;
	struct dst_entry *dst;
	int err;

	route = kzalloc_obj(*route, GFP_ATOMIC);
	if (!route) {
		hsk->error_msg = "couldn't allocate memory for homa_route";
		return ERR_PTR(-ENOMEM);
	}
	spin_lock_init(&route->lock);
	refcount_set(&route->refs, 1);
	route->access_jiffies = jiffies;
	route->key = *key;

	/* Find a dst_entry to use for this route. */
	route->flow.flowi_secid = key->secid;
	if (ipv6_addr_v4mapped(&route->key.daddr)) {
		struct rtable *rt;

		flowi4_init_output(&route->flow.u.ip4, key->bound_dev_if,
				   key->mark, 0, RT_SCOPE_UNIVERSE,
				   IPPROTO_HOMA, 0,
				   ipv6_to_ipv4(route->key.daddr),
				   ipv6_to_ipv4(route->key.saddr), 0, 0,
				   key->uid);
		rt = ip_route_output_flow(sock_net(&hsk->sock),
					  &route->flow.u.ip4, &hsk->sock);
		if (IS_ERR(rt)) {
			err = PTR_ERR(rt);
			goto error;
		}
		rcu_assign_pointer(route->dst, &rt->dst);
	} else {
		/* This code is derived from code in tcp_v6_connect. */
		route->flow.u.ip6.flowi6_proto = IPPROTO_HOMA;
		route->flow.u.ip6.daddr = route->key.daddr;
		route->flow.u.ip6.saddr = route->key.saddr;
		route->flow.u.ip6.flowlabel = ip6_make_flowinfo(0, 0);
		route->flow.u.ip6.flowi6_oif = key->bound_dev_if;
		route->flow.u.ip6.flowi6_mark = key->mark;
		route->flow.u.ip6.fl6_dport = 0;
		route->flow.u.ip6.fl6_sport = 0;
		route->flow.u.ip6.flowi6_uid = key->uid;
		dst = ip6_dst_lookup_flow(sock_net(&hsk->sock), &hsk->sock,
						   &route->flow.u.ip6, NULL);
		rcu_assign_pointer(route->dst, dst);

		if (IS_ERR(route->dst)) {
			err = PTR_ERR(route->dst);
			goto error;
		}
		route->dst_cookie = rt6_get_cookie(dst_rt6_info(dst));
	}
	return route;
error:
	hsk->error_msg = "couldn't find route for peer";
	kfree(route);
	return ERR_PTR(err);
}

/**
 * homa_route_free() - Release any resources in a homa_route and
 * free the object's memory. May be invoked either as an RCU callback
 * or directly (if invoked directly, caller must ensure exclusive
 * access to the object, e.g. it was never actually published in
 * route_ht).
 * @head:     @rcu_head field  in the route to free.
 */
void homa_route_free(struct rcu_head *head)
{
	struct homa_route *route;

	route = container_of(head, struct homa_route, rcu_head);
	dst_release(rcu_dereference_protected(route->dst, 1));
	spin_lock_bh(&route->key.hnet->homa->peertab->lock);
	homa_peer_unlink(route);
	spin_unlock_bh(&route->key.hnet->homa->peertab->lock);
	kfree(route);
}

/**
 * homa_route_get() - Returns a homa_route object that can be
 * used to communicate with a given host over a given socket. Indirectly
 * provides access to a homa_peer object for @addr.
 * @hsk:        Socket where the peer will be used.
 * @addr:       Address of the desired host: IPv4 addresses are represented
 *              as IPv4-mapped IPv6 addresses.
 *
 * Return:      The homa_route associated with @addr, or a negative
 *              errno if an error occurred. On a successful return the
 *              reference count will be incremented for the returned object.
 *              The caller must eventually call homa_peer_route_release to
 *              release the reference.
 */
struct homa_route *homa_route_get(struct homa_sock *hsk,
				  const struct in6_addr *addr)
{
	struct homa_peertab *peertab = hsk->homa->peertab;
	struct homa_route *route, *other;
	struct homa_route_key route_key;
	struct homa_peer *peer;

	/* See if we already have a suitable route cached. */
	rcu_read_lock();
	homa_route_key_init(&route_key, hsk, addr);
	route = rhashtable_lookup(&peertab->route_ht, &route_key,
				  route_ht_params);
	if (route && refcount_inc_not_zero(&route->refs)) {
		route->access_jiffies = jiffies;
		rcu_read_unlock();
		return route;
	}

	/* No existing entry, so we have to create a new one. Switch from
	 * RCU to real locking for this.
	 */
	rcu_read_unlock();
	route = homa_route_alloc(hsk, &route_key);
	if (IS_ERR(route))
		return route;

	spin_lock_bh(&peertab->lock);
	peer = homa_peer_get(hsk, addr);
	if (IS_ERR(peer)) {
		spin_unlock_bh(&peertab->lock);
		homa_route_free(&route->rcu_head);
		route = ERR_PTR(PTR_ERR(peer));
		return route;
	}
	route->peer = peer;

	/* Insert the new entry in the flow table. It's possible that
	 * someone else already created the entry concurrently.
	 */
	other = rhashtable_lookup_get_insert_fast(&peertab->route_ht,
						  &route->ht_linkage,
						  route_ht_params);
	if (IS_ERR(other)) {
		/* Couldn't insert; return the error info. */
		hsk->error_msg = "rhashtable_lookup_get_insert_key failed in homa_route_get";
		spin_unlock_bh(&peertab->lock);
		homa_route_free(&route->rcu_head);
		route = other;
	} else if (other) {
		/* Someone else already created the desired object; use that
		 * one instead of ours.
		 */
		refcount_inc(&other->refs);
		spin_unlock_bh(&peertab->lock);
		homa_route_free(&route->rcu_head);
		route = other;
		route->access_jiffies = jiffies;
	} else {
		/* The new object was inserted in the hash table. */
		refcount_inc(&route->refs);
		peertab->num_routes++;
		route_key.hnet->num_routes++;
		spin_unlock_bh(&peertab->lock);
	}
	return route;
}

/**
 * homa_route_validate() - Check to be sure that the information in the
 * route for an RPC is still valid; if not, try to allocate a new route
 * for the RPC.
 * @rpc:    RPC whose routing information should be checked.
 * Return:  Zero for success, or a negative errno if the route was invalid
 *          and no new route could be created.
 */
int homa_route_validate(struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	struct homa_route *route, *old;
	struct dst_entry *dst;

	old = rpc->route;
	rcu_read_lock();
	dst = rcu_dereference_protected(old->dst, 1);
	rcu_read_unlock();
	if (!dst_check(dst, rpc->route->dst_cookie)) {
		struct homa_peertab *peertab = rpc->hsk->homa->peertab;

		/* Existing route is no longer valid; remove it from
		 * the hash table and try to create a new one.
		 */
		spin_lock_bh(&peertab->lock);
		if (rhashtable_remove_fast(&peertab->route_ht, &old->ht_linkage,
					   route_ht_params) == 0) {
			homa_route_release(old);
			peertab->num_routes--;
			old->key.hnet->num_routes--;
		}
		spin_unlock_bh(&peertab->lock);
		route = homa_route_get(rpc->hsk, &rpc->route->peer->addr);
		if (IS_ERR(route))
			return PTR_ERR(route);
		old = xchg(&rpc->route, route);
		homa_route_release(old);
	}
	return 0;
}

/**
 * homa_route_gc() - This function is invoked by Homa at regular intervals;
 * its job is to ensure that the number of routes stays within limits. If
 * the number grows too large, it selectively deletes routes (and peers)
 * to get back under the limit.
 * @peertab:   Structure to garbage-collect.
 */
void homa_route_gc(struct homa_peertab *peertab)
{
#define EVICT_BATCH_SIZE 5
	struct homa_route *victims[EVICT_BATCH_SIZE];
	int num_victims;
	int i;

	spin_lock_bh(&peertab->lock);
	if (peertab->num_routes < peertab->gc_threshold)
		goto done;
	num_victims = homa_route_pick_victims(peertab, victims,
					      EVICT_BATCH_SIZE);
	if (num_victims == 0)
		goto done;

	for (i = 0; i < num_victims; i++) {
		struct homa_route *route = victims[i];

		if (rhashtable_remove_fast(&peertab->route_ht,
					   &route->ht_linkage,
					   route_ht_params) == 0) {
			peertab->num_routes--;
			route->key.hnet->num_routes--;
			homa_route_release(route);
		}
	}
done:
	spin_unlock_bh(&peertab->lock);
}

/**
 * homa_route_pick_victims() - Select a few routes that can be freed.
 * @peertab:      Choose routes that are stored here.
 * @victims:      Return addresses of victims here.
 * @max_victims:  Limit on how many victims to choose (and size of @victims
 *                array).
 * Return:        The number of routes stored in @victims; may be zero.
 */
int homa_route_pick_victims(struct homa_peertab *peertab,
			    struct homa_route *victims[], int max_victims)
{
	struct homa_route *route;
	int num_victims = 0;
	int to_scan;
	int i, idle;

	/* Scan 2 routes for every potential victim and keep the "best"
	 * routes for removal.
	 */
	rhashtable_walk_start(&peertab->route_ht_iter);
	for (to_scan = 2 * max_victims; to_scan > 0; to_scan--) {
		route = rhashtable_walk_next(&peertab->route_ht_iter);
		if (!route) {
			/* Reached the end of the table; restart at
			 * the beginning.
			 */
			rhashtable_walk_stop(&peertab->route_ht_iter);
			rhashtable_walk_exit(&peertab->route_ht_iter);
			rhashtable_walk_enter(&peertab->peer_ht,
					      &peertab->route_ht_iter);
			rhashtable_walk_start(&peertab->route_ht_iter);
			route = rhashtable_walk_next(&peertab->route_ht_iter);
			if (!route)
				break;
		}
		if (IS_ERR(route)) {
			/* rhashtable decided to restart the search at the
			 * beginning.
			 */
			route = rhashtable_walk_next(&peertab->route_ht_iter);
			if (!route || IS_ERR(route))
				break;
		}

		/* Has this route been idle long enough to be candidate for
		 * eviction?
		 */
		idle = jiffies - route->access_jiffies;
		if (idle < peertab->idle_jiffies_min)
			continue;
		if (idle < peertab->idle_jiffies_max &&
		    route->key.hnet->num_routes <= peertab->net_max)
			continue;

		/* Sort the candidate into the existing list of victims. */
		for (i = 0; i < num_victims; i++) {
			if (route == victims[i]) {
				/* This can happen if there aren't very many
				 * routes and we wrapped around in the hash
				 * table.
				 */
				route = NULL;
				break;
			}
			if (homa_route_prefer_evict(peertab, route, victims[i]))
				swap(route, victims[i]);
		}

		if (num_victims < max_victims && route) {
			victims[num_victims] = route;
			num_victims++;
		}
	}
	rhashtable_walk_stop(&peertab->route_ht_iter);
	return num_victims;
}

/**
 * homa_route_prefer_evict() - Given two routes, determine which one is
 * a better candidate for eviction.
 * @peertab:    Overall information used to manage routes.
 * @route1:     First route.
 * @route2:     Second route.
 * Return:      True if @route1 is a better candidate for eviction than @route2.
 */
int homa_route_prefer_evict(struct homa_peertab *peertab,
			    struct homa_route *route1,
			    struct homa_route *route2)
{
	/* Prefer a route whose homa_net is over its limit; if both are either
	 * over or under, then prefer the route with the longest idle time.
	 */
	if (route1->key.hnet->num_routes > peertab->net_max) {
		if (route2->key.hnet->num_routes <= peertab->net_max)
			return true;
		else
			return time_before(route1->access_jiffies,
					   route2->access_jiffies);
	}
	if (route2->key.hnet->num_routes > peertab->net_max)
		return false;
	else
		return time_before(route1->access_jiffies,
				   route2->access_jiffies);
}

/**
 * homa_peer_add_ack() - Add a given RPC to the list of unacked
 * RPCs for its server. Once this method has been invoked, it's safe
 * to delete the RPC, since it will eventually be acked to the server.
 * @rpc:    Client RPC that has now completed. Must be locked by caller.
 */
void homa_peer_add_ack(struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	struct homa_peer *peer = rpc->route->peer;
	struct homa_ack_hdr ack;

	homa_peer_lock(peer);
	if (peer->num_acks < HOMA_MAX_ACKS_PER_PKT) {
		peer->acks[peer->num_acks].client_id = cpu_to_be64(rpc->id);
		peer->acks[peer->num_acks].server_port = htons(rpc->dport);
		peer->num_acks++;
		homa_peer_unlock(peer);
		return;
	}

	/* The peer has filled up; send an ACK message to empty it. The
	 * RPC in the message header will also be considered ACKed.
	 */
	memcpy(ack.acks, peer->acks, sizeof(peer->acks));
	ack.num_acks = htons(peer->num_acks);
	peer->num_acks = 0;
	homa_peer_unlock(peer);
	homa_xmit_control(ACK, &ack, sizeof(ack), rpc);
}

/**
 * homa_peer_get_acks() - Copy acks out of a peer, and remove them from the
 * peer.
 * @peer:    Peer to check for possible unacked RPCs.
 * @count:   Maximum number of acks to return.
 * @dst:     The acks are copied to this location.
 *
 * Return:   The number of acks extracted from the peer (<= count).
 */
int homa_peer_get_acks(struct homa_peer *peer, int count, struct homa_ack *dst)
{
	/* Don't waste time acquiring the lock if there are no ids available. */
	if (peer->num_acks == 0)
		return 0;

	homa_peer_lock(peer);

	if (count > peer->num_acks)
		count = peer->num_acks;
	memcpy(dst, &peer->acks[peer->num_acks - count],
	       count * sizeof(peer->acks[0]));
	peer->num_acks -= count;

	homa_peer_unlock(peer);
	return count;
}

/**
 * homa_peer_update_sysctl_deps() - Update any peertab fields that depend
 * on values set by sysctl. This function is invoked anytime a peer sysctl
 * value is updated.
 * @peertab:   Struct to update.
 */
void homa_peer_update_sysctl_deps(struct homa_peertab *peertab)
{
	peertab->idle_jiffies_min = peertab->idle_secs_min * HZ;
	peertab->idle_jiffies_max = peertab->idle_secs_max * HZ;
}
