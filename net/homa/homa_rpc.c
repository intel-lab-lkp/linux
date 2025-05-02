// SPDX-License-Identifier: BSD-2-Clause

/* This file contains functions for managing homa_rpc structs. */

#include "homa_impl.h"
#include "homa_interest.h"
#include "homa_pacer.h"
#include "homa_peer.h"
#include "homa_pool.h"
#include "homa_stub.h"

/**
 * homa_rpc_new_client() - Allocate and construct a client RPC (one that is used
 * to issue an outgoing request). Doesn't send any packets. Invoked with no
 * locks held.
 * @hsk:      Socket to which the RPC belongs.
 * @dest:     Address of host (ip and port) to which the RPC will be sent.
 *
 * Return:    A printer to the newly allocated object, or a negative
 *            errno if an error occurred. The RPC will be locked; the
 *            caller must eventually unlock it.
 */
struct homa_rpc *homa_rpc_new_client(struct homa_sock *hsk,
				     const union sockaddr_in_union *dest)
	__acquires(rpc_bucket_lock)
{
	struct in6_addr dest_addr_as_ipv6 = canonical_ipv6_addr(dest);
	struct homa_rpc_bucket *bucket;
	struct homa_rpc *crpc;
	int err;

	crpc = kmalloc(sizeof(*crpc), GFP_KERNEL | __GFP_ZERO);
	if (unlikely(!crpc))
		return ERR_PTR(-ENOMEM);

	/* Initialize fields that don't require the socket lock. */
	crpc->hsk = hsk;
	crpc->id = atomic64_fetch_add(2, &hsk->homa->next_outgoing_id);
	bucket = homa_client_rpc_bucket(hsk, crpc->id);
	crpc->bucket = bucket;
	crpc->state = RPC_OUTGOING;
	crpc->peer = homa_peer_find(hsk->homa->peers, &dest_addr_as_ipv6,
				    &hsk->inet);
	if (IS_ERR(crpc->peer)) {
		err = PTR_ERR(crpc->peer);
		goto error;
	}
	crpc->dport = ntohs(dest->in6.sin6_port);
	crpc->msgin.length = -1;
	crpc->msgout.length = -1;
	INIT_LIST_HEAD(&crpc->ready_links);
	INIT_LIST_HEAD(&crpc->buf_links);
	INIT_LIST_HEAD(&crpc->dead_links);
	INIT_LIST_HEAD(&crpc->throttled_links);
	crpc->resend_timer_ticks = hsk->homa->timer_ticks;
	crpc->magic = HOMA_RPC_MAGIC;
	crpc->start_ns = sched_clock();

	/* Initialize fields that require locking. This allows the most
	 * expensive work, such as copying in the message from user space,
	 * to be performed without holding locks. Also, can't hold spin
	 * locks while doing things that could block, such as memory allocation.
	 */
	homa_bucket_lock(bucket, crpc->id);
	homa_sock_lock(hsk);
	if (hsk->shutdown) {
		homa_sock_unlock(hsk);
		homa_rpc_unlock(crpc);
		err = -ESHUTDOWN;
		goto error;
	}
	hlist_add_head(&crpc->hash_links, &bucket->rpcs);
	rcu_read_lock();
	list_add_tail_rcu(&crpc->active_links, &hsk->active_rpcs);
	rcu_read_unlock();
	homa_sock_unlock(hsk);

	return crpc;

error:
	kfree(crpc);
	return ERR_PTR(err);
}

/**
 * homa_rpc_new_server() - Allocate and construct a server RPC (one that is
 * used to manage an incoming request). If appropriate, the RPC will also
 * be handed off (we do it here, while we have the socket locked, to avoid
 * acquiring the socket lock a second time later for the handoff).
 * @hsk:      Socket that owns this RPC.
 * @source:   IP address (network byte order) of the RPC's client.
 * @h:        Header for the first data packet received for this RPC; used
 *            to initialize the RPC.
 * @created:  Will be set to 1 if a new RPC was created and 0 if an
 *            existing RPC was found.
 *
 * Return:  A pointer to a new RPC, which is locked, or a negative errno
 *          if an error occurred. If there is already an RPC corresponding
 *          to h, then it is returned instead of creating a new RPC.
 */
struct homa_rpc *homa_rpc_new_server(struct homa_sock *hsk,
				     const struct in6_addr *source,
				     struct homa_data_hdr *h, int *created)
	__acquires(rpc_bucket_lock)
{
	u64 id = homa_local_id(h->common.sender_id);
	struct homa_rpc_bucket *bucket;
	struct homa_rpc *srpc = NULL;
	int err;

	/* Lock the bucket, and make sure no-one else has already created
	 * the desired RPC.
	 */
	bucket = homa_server_rpc_bucket(hsk, id);
	homa_bucket_lock(bucket, id);
	hlist_for_each_entry(srpc, &bucket->rpcs, hash_links) {
		if (srpc->id == id &&
		    srpc->dport == ntohs(h->common.sport) &&
		    ipv6_addr_equal(&srpc->peer->addr, source)) {
			/* RPC already exists; just return it instead
			 * of creating a new RPC.
			 */
			*created = 0;
			return srpc;
		}
	}

	/* Initialize fields that don't require the socket lock. */
	srpc = kmalloc(sizeof(*srpc), GFP_ATOMIC | __GFP_ZERO);
	if (!srpc) {
		err = -ENOMEM;
		goto error;
	}
	srpc->hsk = hsk;
	srpc->bucket = bucket;
	srpc->state = RPC_INCOMING;
	srpc->peer = homa_peer_find(hsk->homa->peers, source, &hsk->inet);
	if (IS_ERR(srpc->peer)) {
		err = PTR_ERR(srpc->peer);
		goto error;
	}
	srpc->dport = ntohs(h->common.sport);
	srpc->id = id;
	srpc->msgin.length = -1;
	srpc->msgout.length = -1;
	INIT_LIST_HEAD(&srpc->ready_links);
	INIT_LIST_HEAD(&srpc->buf_links);
	INIT_LIST_HEAD(&srpc->dead_links);
	INIT_LIST_HEAD(&srpc->throttled_links);
	srpc->resend_timer_ticks = hsk->homa->timer_ticks;
	srpc->magic = HOMA_RPC_MAGIC;
	srpc->start_ns = sched_clock();
	err = homa_message_in_init(srpc, ntohl(h->message_length));
	if (err != 0)
		goto error;

	/* Initialize fields that require socket to be locked. */
	homa_sock_lock(hsk);
	if (hsk->shutdown) {
		homa_sock_unlock(hsk);
		err = -ESHUTDOWN;
		goto error;
	}
	hlist_add_head(&srpc->hash_links, &bucket->rpcs);
	list_add_tail_rcu(&srpc->active_links, &hsk->active_rpcs);
	homa_sock_unlock(hsk);
	if (ntohl(h->seg.offset) == 0 && srpc->msgin.num_bpages > 0) {
		atomic_or(RPC_PKTS_READY, &srpc->flags);
		homa_rpc_handoff(srpc);
	}
	*created = 1;
	return srpc;

error:
	homa_bucket_unlock(bucket, id);
	kfree(srpc);
	return ERR_PTR(err);
}

/**
 * homa_rpc_acked() - This function is invoked when an ack is received
 * for an RPC; if the RPC still exists, is freed.
 * @hsk:     Socket on which the ack was received. May or may not correspond
 *           to the RPC, but can sometimes be used to avoid a socket lookup.
 * @saddr:   Source address from which the act was received (the client
 *           note for the RPC)
 * @ack:     Information about an RPC from @saddr that may now be deleted
 *           safely.
 */
void homa_rpc_acked(struct homa_sock *hsk, const struct in6_addr *saddr,
		    struct homa_ack *ack)
{
	__u16 server_port = ntohs(ack->server_port);
	u64 id = homa_local_id(ack->client_id);
	struct homa_sock *hsk2 = hsk;
	struct homa_rpc *rpc;

	if (hsk->port != server_port) {
		/* Without RCU, sockets other than hsk can be deleted
		 * out from under us.
		 */
		hsk2 = homa_sock_find(hsk->homa->port_map, server_port);
		if (!hsk2)
			return;
	}
	rpc = homa_find_server_rpc(hsk2, saddr, id);
	if (rpc) {
		homa_rpc_end(rpc);
		homa_rpc_unlock(rpc); /* Locked by homa_find_server_rpc. */
	}
	if (hsk->port != server_port)
		sock_put(&hsk2->sock);
}

/**
 * homa_rpc_end() - Stop all activity on an RPC and begin the process of
 * releasing its resources; this process will continue in the background
 * until homa_rpc_reap eventually completes it.
 * @rpc:  Structure to clean up, or NULL. Must be locked. Its socket must
 *        not be locked. Once this function returns the caller should not
 *        use the RPC except to unlock it.
 */
void homa_rpc_end(struct homa_rpc *rpc)
	__must_hold(rpc_bucket_lock)
{
	/* The goal for this function is to make the RPC inaccessible,
	 * so that no other code will ever access it again. However, don't
	 * actually release resources; leave that to homa_rpc_reap, which
	 * runs later. There are two reasons for this. First, releasing
	 * resources may be expensive, so we don't want to keep the caller
	 * waiting; homa_rpc_reap will run in situations where there is time
	 * to spare. Second, there may be other code that currently has
	 * pointers to this RPC but temporarily released the lock (e.g. to
	 * copy data to/from user space). It isn't safe to clean up until
	 * that code has finished its work and released any pointers to the
	 * RPC (homa_rpc_reap will ensure that this has happened). So, this
	 * function should only make changes needed to make the RPC
	 * inaccessible.
	 */
	if (!rpc || rpc->state == RPC_DEAD)
		return;
	rpc->state = RPC_DEAD;
	rpc->error = -EINVAL;

	/* Unlink from all lists, so no-one will ever find this RPC again. */
	homa_sock_lock(rpc->hsk);
	__hlist_del(&rpc->hash_links);
	list_del_rcu(&rpc->active_links);
	list_add_tail(&rpc->dead_links, &rpc->hsk->dead_rpcs);
	__list_del_entry(&rpc->ready_links);
	__list_del_entry(&rpc->buf_links);
	homa_interest_notify_private(rpc);

	if (rpc->msgin.length >= 0) {
		rpc->hsk->dead_skbs += skb_queue_len(&rpc->msgin.packets);
		while (1) {
			struct homa_gap *gap;

			gap = list_first_entry_or_null(&rpc->msgin.gaps,
						       struct homa_gap, links);
			if (!gap)
				break;
			list_del(&gap->links);
			kfree(gap);
		}
	}
	rpc->hsk->dead_skbs += rpc->msgout.num_skbs;
	if (rpc->hsk->dead_skbs > rpc->hsk->homa->max_dead_buffs)
		/* This update isn't thread-safe; it's just a
		 * statistic so it's OK if updates occasionally get
		 * missed.
		 */
		rpc->hsk->homa->max_dead_buffs = rpc->hsk->dead_skbs;

	homa_sock_unlock(rpc->hsk);
	homa_pacer_unmanage_rpc(rpc);
}

/**
 * homa_rpc_reap() - Invoked to release resources associated with dead
 * RPCs for a given socket. For a large RPC, it can take a long time to
 * free all of its packet buffers, so we try to perform this work
 * off the critical path where it won't delay applications. Each call to
 * this function normally does a small chunk of work (unless reap_all is
 * true). See the file reap.txt for more information.
 * @hsk:      Homa socket that may contain dead RPCs. Must not be locked by the
 *            caller; this function will lock and release.
 * @reap_all: False means do a small chunk of work; there may still be
 *            unreaped RPCs on return. True means reap all dead rpcs for
 *            hsk.  Will busy-wait if reaping has been disabled for some RPCs.
 *
 * Return: A return value of 0 means that we ran out of work to do; calling
 *         again will do no work (there could be unreaped RPCs, but if so,
 *         reaping has been disabled for them).  A value greater than
 *         zero means there is still more reaping work to be done.
 */
int homa_rpc_reap(struct homa_sock *hsk, bool reap_all)
{
#define BATCH_MAX 20
	struct homa_rpc *rpcs[BATCH_MAX];
	struct sk_buff *skbs[BATCH_MAX];
	int num_skbs, num_rpcs;
	struct homa_rpc *rpc;
	struct homa_rpc *tmp;
	int i, batch_size;
	int skbs_to_reap;
	int rx_frees;
	int result = 0;

	/* Each iteration through the following loop will reap
	 * BATCH_MAX skbs.
	 */
	skbs_to_reap = hsk->homa->reap_limit;
	while (skbs_to_reap > 0 && !list_empty(&hsk->dead_rpcs)) {
		batch_size = BATCH_MAX;
		if (!reap_all) {
			if (batch_size > skbs_to_reap)
				batch_size = skbs_to_reap;
			skbs_to_reap -= batch_size;
		}
		num_skbs = 0;
		num_rpcs = 0;
		rx_frees = 0;

		homa_sock_lock(hsk);
		if (atomic_read(&hsk->protect_count)) {
			homa_sock_unlock(hsk);
			if (reap_all)
				continue;
			return 0;
		}

		/* Collect buffers and freeable RPCs. */
		list_for_each_entry_safe(rpc, tmp, &hsk->dead_rpcs,
					 dead_links) {
			int refs;

			/* Make sure that all outstanding uses of the RPC have
			 * completed. We can only be sure if the reference
			 * count is zero when we're holding the lock. Note:
			 * it isn't safe to block while locking the RPC here,
			 * since we hold the socket lock.
			 */
			if (homa_rpc_try_lock(rpc)) {
				refs = atomic_read(&rpc->refs);
				homa_rpc_unlock(rpc);
			} else {
				refs = 1;
			}
			if (refs != 0)
				continue;
			rpc->magic = 0;

			/* For Tx sk_buffs, collect them here but defer
			 * freeing until after releasing the socket lock.
			 */
			if (rpc->msgout.length >= 0) {
				while (rpc->msgout.packets) {
					skbs[num_skbs] = rpc->msgout.packets;
					rpc->msgout.packets = homa_get_skb_info(
						rpc->msgout.packets)->next_skb;
					num_skbs++;
					rpc->msgout.num_skbs--;
					if (num_skbs >= batch_size)
						goto release;
				}
			}

			/* In the normal case rx sk_buffs will already have been
			 * freed before we got here. Thus it's OK to free
			 * immediately in rare situations where there are
			 * buffers left.
			 */
			if (rpc->msgin.length >= 0 &&
			    !skb_queue_empty_lockless(&rpc->msgin.packets)) {
				rx_frees += skb_queue_len(&rpc->msgin.packets);
				__skb_queue_purge(&rpc->msgin.packets);
			}

			/* If we get here, it means all packets have been
			 *  removed from the RPC.
			 */
			rpcs[num_rpcs] = rpc;
			num_rpcs++;
			list_del(&rpc->dead_links);
			WARN_ON(refcount_sub_and_test(rpc->msgout.skb_memory,
						      &hsk->sock.sk_wmem_alloc));
			if (num_rpcs >= batch_size)
				goto release;
		}

		/* Free all of the collected resources; release the socket
		 * lock while doing this.
		 */
release:
		hsk->dead_skbs -= num_skbs + rx_frees;
		result = !list_empty(&hsk->dead_rpcs) &&
				(num_skbs + num_rpcs) != 0;
		homa_sock_unlock(hsk);
		homa_skb_free_many_tx(hsk->homa, skbs, num_skbs);
		for (i = 0; i < num_rpcs; i++) {
			rpc = rpcs[i];

			if (unlikely(rpc->msgin.num_bpages))
				homa_pool_release_buffers(rpc->hsk->buffer_pool,
							  rpc->msgin.num_bpages,
							  rpc->msgin.bpage_offsets);
			if (rpc->msgin.length >= 0) {
				while (1) {
					struct homa_gap *gap;

					gap = list_first_entry_or_null(
							&rpc->msgin.gaps,
							struct homa_gap,
							links);
					if (!gap)
						break;
					list_del(&gap->links);
					kfree(gap);
				}
			}
			rpc->state = 0;
			kfree(rpc);
		}
		homa_sock_wakeup_wmem(hsk);
		if (!result && !reap_all)
			break;
	}
	homa_pool_check_waiting(hsk->buffer_pool);
	return result;
}

/**
 * homa_find_client_rpc() - Locate client-side information about the RPC that
 * a packet belongs to, if there is any. Thread-safe without socket lock.
 * @hsk:      Socket via which packet was received.
 * @id:       Unique identifier for the RPC.
 *
 * Return:    A pointer to the homa_rpc for this id, or NULL if none.
 *            The RPC will be locked; the caller must eventually unlock it
 *            by invoking homa_rpc_unlock.
 */
struct homa_rpc *homa_find_client_rpc(struct homa_sock *hsk, u64 id)
	__cond_acquires(rpc_bucket_lock)
{
	struct homa_rpc_bucket *bucket = homa_client_rpc_bucket(hsk, id);
	struct homa_rpc *crpc;

	homa_bucket_lock(bucket, id);
	hlist_for_each_entry(crpc, &bucket->rpcs, hash_links) {
		if (crpc->id == id)
			return crpc;
	}
	homa_bucket_unlock(bucket, id);
	return NULL;
}

/**
 * homa_find_server_rpc() - Locate server-side information about the RPC that
 * a packet belongs to, if there is any. Thread-safe without socket lock.
 * @hsk:      Socket via which packet was received.
 * @saddr:    Address from which the packet was sent.
 * @id:       Unique identifier for the RPC (must have server bit set).
 *
 * Return:    A pointer to the homa_rpc matching the arguments, or NULL
 *            if none. The RPC will be locked; the caller must eventually
 *            unlock it by invoking homa_rpc_unlock.
 */
struct homa_rpc *homa_find_server_rpc(struct homa_sock *hsk,
				      const struct in6_addr *saddr, u64 id)
	__cond_acquires(rpc_bucket_lock)
{
	struct homa_rpc_bucket *bucket = homa_server_rpc_bucket(hsk, id);
	struct homa_rpc *srpc;

	homa_bucket_lock(bucket, id);
	hlist_for_each_entry(srpc, &bucket->rpcs, hash_links) {
		if (srpc->id == id && ipv6_addr_equal(&srpc->peer->addr, saddr))
			return srpc;
	}
	homa_bucket_unlock(bucket, id);
	return NULL;
}

