// SPDX-License-Identifier: BSD-2-Clause or GPL-2.0+

/* This file provides functions related to homa_peer and homa_peertab
 * objects.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_rpc.h"

static const struct rhashtable_params ht_params = {
	.key_len     = sizeof(struct homa_peer_key),
	.key_offset  = offsetof(struct homa_peer, ht_key),
	.head_offset = offsetof(struct homa_peer, ht_linkage),
	.nelem_hint = 10000,
	.hashfn = homa_peer_hash,
	.obj_cmpfn = homa_peer_compare
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

	peertab = kzalloc(sizeof(*peertab), GFP_KERNEL);
	if (!peertab)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&peertab->lock);
	err = rhashtable_init(&peertab->ht, &ht_params);
	if (err) {
		kfree(peertab);
		return ERR_PTR(err);
	}
	peertab->ht_valid = true;
	rhashtable_walk_enter(&peertab->ht, &peertab->ht_iter);
	INIT_LIST_HEAD(&peertab->dead_peers);
	peertab->gc_threshold = 5000;
	peertab->net_max = 10000;
	peertab->idle_secs_min = 10;
	peertab->idle_secs_max = 120;

	homa_peer_update_sysctl_deps(peertab);
	return peertab;
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
	struct homa_peer *peer;

	spin_lock_bh(&peertab->lock);
	peertab->gc_stop_count++;
	spin_unlock_bh(&peertab->lock);

	rhashtable_walk_enter(&peertab->ht, &iter);
	rhashtable_walk_start(&iter);
	while (1) {
		peer = rhashtable_walk_next(&iter);
		if (!peer)
			break;
		if (IS_ERR(peer))
			continue;
		if (peer->ht_key.hnet != hnet)
			continue;
		if (rhashtable_remove_fast(&peertab->ht, &peer->ht_linkage,
					   ht_params) == 0) {
			homa_peer_free(peer);
			hnet->num_peers--;
			peertab->num_peers--;
		}
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	WARN(hnet->num_peers != 0, "%s ended up with hnet->num_peers %d",
	     __func__, hnet->num_peers);

	spin_lock_bh(&peertab->lock);
	peertab->gc_stop_count--;
	spin_unlock_bh(&peertab->lock);
}

/**
 * homa_peer_free_fn() - This function is invoked for each entry in
 * the peer hash table by the rhashtable code when the table is being
 * deleted. It frees its argument.
 * @object:     struct homa_peer to free.
 * @dummy:      Not used.
 */
void homa_peer_free_fn(void *object, void *dummy)
{
	struct homa_peer *peer = object;

	homa_peer_free(peer);
}

/**
 * homa_peer_free_peertab() - Destructor for homa_peertabs. After this
 * function returns, it is unsafe to use any results from previous calls
 * to homa_peer_get, since all existing homa_peer objects will have been
 * destroyed.
 * @peertab:  The table to destroy.
 */
void homa_peer_free_peertab(struct homa_peertab *peertab)
{
	spin_lock_bh(&peertab->lock);
	peertab->gc_stop_count++;
	spin_unlock_bh(&peertab->lock);

	if (peertab->ht_valid) {
		rhashtable_walk_exit(&peertab->ht_iter);
		rhashtable_free_and_destroy(&peertab->ht, homa_peer_free_fn,
					    NULL);
	}
	while (!list_empty(&peertab->dead_peers))
		homa_peer_free_dead(peertab);
	kfree(peertab);
}

/**
 * homa_peer_rcu_callback() - This function is invoked as the callback
 * for an invocation of call_rcu. It just marks a peertab to indicate that
 * it was invoked.
 * @head:    Contains information used to locate the peertab.
 */
void homa_peer_rcu_callback(struct rcu_head *head)
{
	struct homa_peertab *peertab;

	peertab = container_of(head, struct homa_peertab, rcu_head);
	atomic_set(&peertab->call_rcu_pending, 0);
}

/**
 * homa_peer_free_dead() - Release peers on peertab->dead_peers
 * if possible.
 * @peertab:    Check the dead peers here.
 */
void homa_peer_free_dead(struct homa_peertab *peertab)
	__must_hold(peertab->lock)
{
	struct homa_peer *peer, *tmp;

	/* A dead peer can be freed only if:
	 * (a) there are no call_rcu calls pending (if there are, it's
	 *     possible that a new reference might get created for the
	 *     peer)
	 * (b) the peer's reference count is zero.
	 */
	if (atomic_read(&peertab->call_rcu_pending))
		return;
	list_for_each_entry_safe(peer, tmp, &peertab->dead_peers, dead_links) {
		if (atomic_read(&peer->refs) == 0) {
			list_del_init(&peer->dead_links);
			homa_peer_free(peer);
		}
	}
}

/**
 * homa_peer_wait_dead() - Don't return until all of the dead peers have
 * been freed.
 * @peertab:    Overall information about peers, which includes a dead list.
 *
 */
void homa_peer_wait_dead(struct homa_peertab *peertab)
{
	while (1) {
		spin_lock_bh(&peertab->lock);
		homa_peer_free_dead(peertab);
		if (list_empty(&peertab->dead_peers)) {
			spin_unlock_bh(&peertab->lock);
			return;
		}
		spin_unlock_bh(&peertab->lock);
	}
}

/**
 * homa_peer_prefer_evict() - Given two peers, determine which one is
 * a better candidate for eviction.
 * @peertab:    Overall information used to manage peers.
 * @peer1:      First peer.
 * @peer2:      Second peer.
 * Return:      True if @peer1 is a better candidate for eviction than @peer2.
 */
int homa_peer_prefer_evict(struct homa_peertab *peertab,
			   struct homa_peer *peer1,
			   struct homa_peer *peer2)
{
	/* Prefer a peer whose homa-net is over its limit; if both are either
	 * over or under, then prefer the peer with the shortest idle time.
	 */
	if (peer1->ht_key.hnet->num_peers > peertab->net_max) {
		if (peer2->ht_key.hnet->num_peers <= peertab->net_max)
			return true;
		else
			return peer1->access_jiffies < peer2->access_jiffies;
	}
	if (peer2->ht_key.hnet->num_peers > peertab->net_max)
		return false;
	else
		return peer1->access_jiffies < peer2->access_jiffies;
}

/**
 * homa_peer_pick_victims() - Select a few peers that can be freed.
 * @peertab:      Choose peers that are stored here.
 * @victims:      Return addresses of victims here.
 * @max_victims:  Limit on how many victims to choose (and size of @victims
 *                array).
 * Return:        The number of peers stored in @victims; may be zero.
 */
int homa_peer_pick_victims(struct homa_peertab *peertab,
			   struct homa_peer *victims[], int max_victims)
{
	struct homa_peer *peer;
	int num_victims = 0;
	int to_scan;
	int i, idle;

	/* Scan 2 peers for every potential victim and keep the "best"
	 * peers for removal.
	 */
	rhashtable_walk_start(&peertab->ht_iter);
	for (to_scan = 2 * max_victims; to_scan > 0; to_scan--) {
		peer = rhashtable_walk_next(&peertab->ht_iter);
		if (!peer) {
			/* Reached the end of the table; restart at
			 * the beginning.
			 */
			rhashtable_walk_stop(&peertab->ht_iter);
			rhashtable_walk_exit(&peertab->ht_iter);
			rhashtable_walk_enter(&peertab->ht, &peertab->ht_iter);
			rhashtable_walk_start(&peertab->ht_iter);
			peer = rhashtable_walk_next(&peertab->ht_iter);
			if (!peer)
				break;
		}
		if (IS_ERR(peer)) {
			/* rhashtable decided to restart the search at the
			 * beginning.
			 */
			peer = rhashtable_walk_next(&peertab->ht_iter);
			if (!peer || IS_ERR(peer))
				break;
		}

		/* Has this peer been idle long enough to be candidate for
		 * eviction?
		 */
		idle = jiffies - peer->access_jiffies;
		if (idle < peertab->idle_jiffies_min)
			continue;
		if (idle < peertab->idle_jiffies_max &&
		    peer->ht_key.hnet->num_peers <= peertab->net_max)
			continue;

		/* Sort the candidate into the existing list of victims. */
		for (i = 0; i < num_victims; i++) {
			if (peer == victims[i]) {
				/* This can happen if there aren't very many
				 * peers and we wrapped around in the hash
				 * table.
				 */
				peer = NULL;
				break;
			}
			if (homa_peer_prefer_evict(peertab, peer, victims[i])) {
				struct homa_peer *tmp;

				tmp = victims[i];
				victims[i] = peer;
				peer = tmp;
			}
		}

		if (num_victims < max_victims && peer) {
			victims[num_victims] = peer;
			num_victims++;
		}
	}
	rhashtable_walk_stop(&peertab->ht_iter);
	return num_victims;
}

/**
 * homa_peer_gc() - This function is invoked by Homa at regular intervals;
 * its job is to ensure that the number of peers stays within limits.
 * If the number grows too large, it selectively deletes peers to get
 * back under the limit.
 * @peertab:   Structure whose peers should be considered for garbage
 *             collection.
 */
void homa_peer_gc(struct homa_peertab *peertab)
{
#define EVICT_BATCH_SIZE 5
	struct homa_peer *victims[EVICT_BATCH_SIZE];
	int num_victims;
	int i;

	spin_lock_bh(&peertab->lock);
	if (peertab->gc_stop_count != 0)
		goto done;
	if (!list_empty(&peertab->dead_peers))
		homa_peer_free_dead(peertab);
	if (atomic_read(&peertab->call_rcu_pending) ||
	    peertab->num_peers < peertab->gc_threshold)
		goto done;
	num_victims = homa_peer_pick_victims(peertab, victims,
					     EVICT_BATCH_SIZE);
	if (num_victims == 0)
		goto done;

	for (i = 0; i < num_victims; i++) {
		struct homa_peer *peer = victims[i];

		if (rhashtable_remove_fast(&peertab->ht, &peer->ht_linkage,
					   ht_params) == 0) {
			list_add_tail(&peer->dead_links, &peertab->dead_peers);
			peertab->num_peers--;
			peer->ht_key.hnet->num_peers--;
		}
	}
	atomic_set(&peertab->call_rcu_pending, 1);
	call_rcu(&peertab->rcu_head, homa_peer_rcu_callback);
done:
	spin_unlock_bh(&peertab->lock);
}

/**
 * homa_peer_alloc() - Allocate and initialize a new homa_peer object.
 * @hsk:        Socket for which the peer will be used.
 * @addr:       Address of the desired host: IPv4 addresses are represented
 *              as IPv4-mapped IPv6 addresses.
 * Return:      The peer associated with @addr, or a negative errno if an
 *              error occurred. On a successful return the reference count
 *              will be incremented for the returned peer.
 */
struct homa_peer *homa_peer_alloc(struct homa_sock *hsk,
				  const struct in6_addr *addr)
{
	struct homa_peer *peer;
	struct dst_entry *dst;

	peer = kzalloc(sizeof(*peer), GFP_ATOMIC);
	if (!peer)
		return (struct homa_peer *)ERR_PTR(-ENOMEM);
	peer->ht_key.addr = *addr;
	peer->ht_key.hnet = hsk->hnet;
	INIT_LIST_HEAD(&peer->dead_links);
	atomic_set(&peer->refs, 1);
	peer->access_jiffies = jiffies;
	peer->addr = *addr;
	dst = homa_peer_get_dst(peer, hsk);
	if (IS_ERR(dst)) {
		kfree(peer);
		return (struct homa_peer *)dst;
	}
	peer->dst = dst;
	peer->current_ticks = -1;
	spin_lock_init(&peer->ack_lock);
	return peer;
}

/**
 * homa_peer_free() - Release any resources in a peer and free the homa_peer
 * struct.
 * @peer:       Structure to free. Must not currently be linked into
 *              peertab->ht.
 */
void homa_peer_free(struct homa_peer *peer)
{
	dst_release(peer->dst);

	if (atomic_read(&peer->refs) == 0)
		kfree(peer);
	else
		WARN(1, "%s found peer with reference count %d",
		     __func__, atomic_read(&peer->refs));
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
{
	struct homa_peertab *peertab = hsk->homa->peertab;
	struct homa_peer *peer, *other;
	struct homa_peer_key key;

	key.addr = *addr;
	key.hnet = hsk->hnet;
	rcu_read_lock();
	peer = rhashtable_lookup(&peertab->ht, &key, ht_params);
	if (peer) {
		homa_peer_hold(peer);
		peer->access_jiffies = jiffies;
		rcu_read_unlock();
		return peer;
	}

	/* No existing entry, so we have to create a new one. */
	peer = homa_peer_alloc(hsk, addr);
	if (IS_ERR(peer)) {
		rcu_read_unlock();
		return peer;
	}
	spin_lock_bh(&peertab->lock);
	other = rhashtable_lookup_get_insert_fast(&peertab->ht,
						  &peer->ht_linkage, ht_params);
	if (IS_ERR(other)) {
		/* Couldn't insert; return the error info. */
		homa_peer_release(peer);
		homa_peer_free(peer);
		peer = other;
	} else if (other) {
		/* Someone else already created the desired peer; use that
		 * one instead of ours.
		 */
		homa_peer_release(peer);
		homa_peer_free(peer);
		peer = other;
		homa_peer_hold(peer);
		peer->access_jiffies = jiffies;
	} else {
		peertab->num_peers++;
		key.hnet->num_peers++;
	}
	spin_unlock_bh(&peertab->lock);
	rcu_read_unlock();
	return peer;
}

/**
 * homa_dst_refresh() - This method is called when the dst for a peer is
 * obsolete; it releases that dst and creates a new one.
 * @peertab:  Table containing the peer.
 * @peer:     Peer whose dst is obsolete.
 * @hsk:      Socket that will be used to transmit data to the peer.
 */
void homa_dst_refresh(struct homa_peertab *peertab, struct homa_peer *peer,
		      struct homa_sock *hsk)
{
	struct dst_entry *dst;

	dst = homa_peer_get_dst(peer, hsk);
	if (IS_ERR(dst))
		return;
	dst_release(peer->dst);
	peer->dst = dst;
}

/**
 * homa_peer_get_dst() - Find an appropriate dst structure (either IPv4
 * or IPv6) for a peer.
 * @peer:   The peer for which a dst is needed. Note: this peer's flow
 *          struct will be overwritten.
 * @hsk:    Socket that will be used for sending packets.
 * Return:  The dst structure (or an ERR_PTR); a reference has been taken.
 */
struct dst_entry *homa_peer_get_dst(struct homa_peer *peer,
				    struct homa_sock *hsk)
{
	memset(&peer->flow, 0, sizeof(peer->flow));
	if (hsk->sock.sk_family == AF_INET) {
		struct rtable *rt;

		flowi4_init_output(&peer->flow.u.ip4, hsk->sock.sk_bound_dev_if,
				   hsk->sock.sk_mark, hsk->inet.tos,
				   RT_SCOPE_UNIVERSE, hsk->sock.sk_protocol, 0,
				   peer->addr.in6_u.u6_addr32[3],
				   hsk->inet.inet_saddr, 0, 0,
				   hsk->sock.sk_uid);
		security_sk_classify_flow(&hsk->sock,
					  &peer->flow.u.__fl_common);
		rt = ip_route_output_flow(sock_net(&hsk->sock),
					  &peer->flow.u.ip4, &hsk->sock);
		if (IS_ERR(rt))
			return (struct dst_entry *)(PTR_ERR(rt));
		return &rt->dst;
	}
	peer->flow.u.ip6.flowi6_oif = hsk->sock.sk_bound_dev_if;
	peer->flow.u.ip6.flowi6_iif = LOOPBACK_IFINDEX;
	peer->flow.u.ip6.flowi6_mark = hsk->sock.sk_mark;
	peer->flow.u.ip6.flowi6_scope = RT_SCOPE_UNIVERSE;
	peer->flow.u.ip6.flowi6_proto = hsk->sock.sk_protocol;
	peer->flow.u.ip6.flowi6_flags = 0;
	peer->flow.u.ip6.flowi6_secid = 0;
	peer->flow.u.ip6.flowi6_tun_key.tun_id = 0;
	peer->flow.u.ip6.flowi6_uid = hsk->sock.sk_uid;
	peer->flow.u.ip6.daddr = peer->addr;
	peer->flow.u.ip6.saddr = hsk->inet.pinet6->saddr;
	peer->flow.u.ip6.fl6_dport = 0;
	peer->flow.u.ip6.fl6_sport = 0;
	peer->flow.u.ip6.mp_hash = 0;
	peer->flow.u.ip6.__fl_common.flowic_tos = hsk->inet.tos;
	peer->flow.u.ip6.flowlabel = ip6_make_flowinfo(hsk->inet.tos, 0);
	security_sk_classify_flow(&hsk->sock, &peer->flow.u.__fl_common);
	return ip6_dst_lookup_flow(sock_net(&hsk->sock), &hsk->sock,
			&peer->flow.u.ip6, NULL);
}

/**
 * homa_peer_add_ack() - Add a given RPC to the list of unacked
 * RPCs for its server. Once this method has been invoked, it's safe
 * to delete the RPC, since it will eventually be acked to the server.
 * @rpc:    Client RPC that has now completed.
 */
void homa_peer_add_ack(struct homa_rpc *rpc)
{
	struct homa_peer *peer = rpc->peer;
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

