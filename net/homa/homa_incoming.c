// SPDX-License-Identifier: BSD-2-Clause

/* This file contains functions that handle incoming Homa messages, including
 * both receiving information for those messages and sending grants.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_pool.h"

/**
 * homa_message_in_init() - Constructor for homa_message_in.
 * @rpc:          RPC whose msgin structure should be initialized.
 * @length:       Total number of bytes in message.
 * Return:        Zero for successful initialization, or a negative errno
 *                if rpc->msgin could not be initialized.
 */
int homa_message_in_init(struct homa_rpc *rpc, int length)
{
	int err;

	rpc->msgin.length = length;
	skb_queue_head_init(&rpc->msgin.packets);
	rpc->msgin.recv_end = 0;
	INIT_LIST_HEAD(&rpc->msgin.gaps);
	rpc->msgin.bytes_remaining = length;
	rpc->msgin.resend_all = 0;
	rpc->msgin.num_bpages = 0;
	err = homa_pool_allocate(rpc);
	if (err != 0)
		return err;
	return 0;
}

/**
 * homa_gap_new() - Create a new gap and add it to a list.
 * @next:   Add the new gap just before this list element.
 * @start:  Offset of first byte covered by the gap.
 * @end:    Offset of byte just after the last one covered by the gap.
 * Return:  Pointer to the new gap.
 */
struct homa_gap *homa_gap_new(struct list_head *next, int start, int end)
{
	struct homa_gap *gap;

	gap = kmalloc(sizeof(*gap), GFP_KERNEL);
	gap->start = start;
	gap->end = end;
	gap->time = get_cycles();
	list_add_tail(&gap->links, next);
	return gap;
}

/**
 * homa_gap_retry() - Send RESEND requests for all of the unreceived
 * gaps in a message.
 * @rpc:     RPC to check; must be locked by caller.
 */
void homa_gap_retry(struct homa_rpc *rpc)
{
	struct resend_header resend;
	struct homa_gap *gap;

	list_for_each_entry(gap, &rpc->msgin.gaps, links) {
		resend.offset = htonl(gap->start);
		resend.length = htonl(gap->end - gap->start);
		homa_xmit_control(RESEND, &resend, sizeof(resend), rpc);
	}
}

/**
 * homa_add_packet() - Add an incoming packet to the contents of a
 * partially received message.
 * @rpc:   Add the packet to the msgin for this RPC.
 * @skb:   The new packet. This function takes ownership of the packet
 *         (the packet will either be freed or added to rpc->msgin.packets).
 */
void homa_add_packet(struct homa_rpc *rpc, struct sk_buff *skb)
{
	struct data_header *h = (struct data_header *)skb->data;
	struct homa_gap *gap, *dummy, *gap2;
	int start = ntohl(h->seg.offset);
	int length = homa_data_len(skb);
	int end = start + length;

	if ((start + length) > rpc->msgin.length)
		goto discard;

	if (start == rpc->msgin.recv_end) {
		/* Common case: packet is sequential. */
		rpc->msgin.recv_end += length;
		goto keep;
	}

	if (start > rpc->msgin.recv_end) {
		/* Packet creates a new gap. */
		homa_gap_new(&rpc->msgin.gaps, rpc->msgin.recv_end, start);
		rpc->msgin.recv_end = end;
		goto keep;
	}

	/* Must now check to see if the packet fills in part or all of
	 * an existing gap.
	 */
	list_for_each_entry_safe(gap, dummy, &rpc->msgin.gaps, links) {
		/* Is packet at the start of this gap? */
		if (start <= gap->start) {
			if (end <= gap->start)
				continue;
			if (start < gap->start)
				goto discard;
			if (end > gap->end)
				goto discard;
			gap->start = end;
			if (gap->start >= gap->end) {
				list_del(&gap->links);
				kfree(gap);
			}
			goto keep;
		}

		/* Is packet at the end of this gap? BTW, at this point we know
		 * the packet can't cover the entire gap.
		 */
		if (end >= gap->end) {
			if (start >= gap->end)
				continue;
			if (end > gap->end)
				goto discard;
			gap->end = start;
			goto keep;
		}

		/* Packet is in the middle of the gap; must split the gap. */
		gap2 = homa_gap_new(&gap->links, gap->start, start);
		gap2->time = gap->time;
		gap->start = end;
		goto keep;
	}

discard:
	kfree_skb(skb);
	return;

keep:
	__skb_queue_tail(&rpc->msgin.packets, skb);
	rpc->msgin.bytes_remaining -= length;
}

/**
 * homa_copy_to_user() - Copy as much data as possible from incoming
 * packet buffers to buffers in user space.
 * @rpc:     RPC for which data should be copied. Must be locked by caller.
 * Return:   Zero for success or a negative errno if there is an error.
 *           It is possible for the RPC to be freed while this function
 *           executes (it releases and reacquires the RPC lock). If that
 *           happens, -EINVAL will be returned and the state of @rpc
 *           will be RPC_DEAD.
 */
int homa_copy_to_user(struct homa_rpc *rpc)
{
#ifdef __UNIT_TEST__
#define MAX_SKBS 3
#else
#define MAX_SKBS 20
#endif
	struct sk_buff *skbs[MAX_SKBS];
	int error = 0;
	int n = 0;             /* Number of filled entries in skbs. */
	int i;

	/* Tricky note: we can't hold the RPC lock while we're actually
	 * copying to user space, because (a) it's illegal to hold a spinlock
	 * while copying to user space and (b) we'd like for homa_softirq
	 * to add more packets to the RPC while we're copying these out.
	 * So, collect a bunch of packets to copy, then release the lock,
	 * copy them, and reacquire the lock.
	 */
	while (true) {
		struct sk_buff *skb;

		if (rpc->state == RPC_DEAD) {
			error = -EINVAL;
			break;
		}

		skb = __skb_dequeue(&rpc->msgin.packets);
		if (skb) {
			skbs[n] = skb;
			n++;
			if (n < MAX_SKBS)
				continue;
		}
		if (n == 0)
			break;

		/* At this point we've collected a batch of packets (or
		 * run out of packets); copy any available packets out to
		 * user space.
		 */
		atomic_or(RPC_COPYING_TO_USER, &rpc->flags);
		homa_rpc_unlock(rpc);

		/* Each iteration of this loop copies out one skb. */
		for (i = 0; i < n; i++) {
			struct data_header *h = (struct data_header *)
					skbs[i]->data;
			int pkt_length = homa_data_len(skbs[i]);
			int offset = ntohl(h->seg.offset);
			int buf_bytes, chunk_size;
			struct iov_iter iter;
			int copied = 0;
			char *dst;

			/* Each iteration of this loop copies to one
			 * user buffer.
			 */
			while (copied < pkt_length) {
				chunk_size = pkt_length - copied;
				dst = homa_pool_get_buffer(rpc, offset + copied,
							   &buf_bytes);
				if (buf_bytes < chunk_size) {
					if (buf_bytes == 0)
						/* skb has data beyond message
						 * end?
						 */
						break;
					chunk_size = buf_bytes;
				}
				error = import_ubuf(READ, dst, chunk_size,
						    &iter);
				if (error)
					goto free_skbs;
				error = skb_copy_datagram_iter(skbs[i],
							       sizeof(*h) + copied,
							       &iter,
							       chunk_size);
				if (error)
					goto free_skbs;
				copied += chunk_size;
			}
		}

free_skbs:
		for (i = 0; i < n; i++)
			kfree_skb(skbs[i]);
		n = 0;
		atomic_or(APP_NEEDS_LOCK, &rpc->flags);
		homa_rpc_lock(rpc, "homa_copy_to_user");
		atomic_andnot(APP_NEEDS_LOCK | RPC_COPYING_TO_USER, &rpc->flags);
		if (error)
			break;
	}
	return error;
}

/**
 * homa_dispatch_pkts() - Top-level function that processes a batch of packets,
 * all related to the same RPC.
 * @skb:       First packet in the batch, linked through skb->next.
 * @homa:      Overall information about the Homa transport.
 */
void homa_dispatch_pkts(struct sk_buff *skb, struct homa *homa)
{
#ifdef __UNIT_TEST__
#define MAX_ACKS 2
#else
#define MAX_ACKS 10
#endif
	const struct in6_addr saddr = skb_canonical_ipv6_saddr(skb);
	struct data_header *h = (struct data_header *)skb->data;
	__u64 id = homa_local_id(h->common.sender_id);
	int dport = ntohs(h->common.dport);

	/* Used to collect acks from data packets so we can process them
	 * all at the end (can't process them inline because that may
	 * require locking conflicting RPCs). If we run out of space just
	 * ignore the extra acks; they'll be regenerated later through the
	 * explicit mechanism.
	 */
	struct homa_ack acks[MAX_ACKS];
	struct homa_rpc *rpc = NULL;
	struct homa_sock *hsk;
	struct sk_buff *next;
	int num_acks = 0;

	/* Find the appropriate socket.*/
	hsk = homa_sock_find(homa->port_map, dport);
	if (!hsk) {
		if (skb_is_ipv6(skb))
			icmp6_send(skb, ICMPV6_DEST_UNREACH,
				   ICMPV6_PORT_UNREACH, 0, NULL, IP6CB(skb));
		else
			icmp_send(skb, ICMP_DEST_UNREACH,
				  ICMP_PORT_UNREACH, 0);
		while (skb) {
			next = skb->next;
			kfree_skb(skb);
			skb = next;
		}
		return;
	}

	/* Each iteration through the following loop processes one packet. */
	for (; skb; skb = next) {
		h = (struct data_header *)skb->data;
		next = skb->next;

		/* Relinquish the RPC lock temporarily if it's needed
		 * elsewhere.
		 */
		if (rpc) {
			int flags = atomic_read(&rpc->flags);

			if (flags & APP_NEEDS_LOCK) {
				homa_rpc_unlock(rpc);
				homa_spin(200);
				rpc = NULL;
			}
		}

		/* Find and lock the RPC if we haven't already done so. */
		if (!rpc) {
			if (!homa_is_client(id)) {
				/* We are the server for this RPC. */
				if (h->common.type == DATA) {
					int created;

					/* Create a new RPC if one doesn't
					 * already exist.
					 */
					rpc = homa_rpc_new_server(hsk, &saddr,
								  h, &created);
					if (IS_ERR(rpc)) {
						pr_warn("homa_pkt_dispatch couldn't create server rpc: error %lu",
							-PTR_ERR(rpc));
						rpc = NULL;
						goto discard;
					}
				} else {
					rpc = homa_find_server_rpc(hsk, &saddr,
								   ntohs(h->common.sport),
								   id);
				}
			} else {
				rpc = homa_find_client_rpc(hsk, id);
			}
		}
		if (unlikely(!rpc)) {
			if (h->common.type != NEED_ACK &&
			    h->common.type != ACK && h->common.type != RESEND)
				goto discard;
		} else {
			if (h->common.type == DATA || h->common.type == BUSY ||
			    h->common.type == NEED_ACK)
				rpc->silent_ticks = 0;
			rpc->peer->outstanding_resends = 0;
		}

		switch (h->common.type) {
		case DATA:
			if (h->ack.client_id) {
				/* Save the ack for processing later, when we
				 * have released the RPC lock.
				 */
				if (num_acks < MAX_ACKS) {
					acks[num_acks] = h->ack;
					num_acks++;
				}
			}
			homa_data_pkt(skb, rpc);
			break;
		case RESEND:
			homa_resend_pkt(skb, rpc, hsk);
			break;
		case UNKNOWN:
			homa_unknown_pkt(skb, rpc);
			break;
		case BUSY:
			/* Nothing to do for these packets except reset
			 * silent_ticks, which happened above.
			 */
			goto discard;
		case NEED_ACK:
			homa_need_ack_pkt(skb, hsk, rpc);
			break;
		case ACK:
			homa_ack_pkt(skb, hsk, rpc);
			rpc = NULL;

			/* It isn't safe to process more packets once we've
			 * released the RPC lock (this should never happen).
			 */
			BUG_ON(next);
			break;
		default:
			goto discard;
		}
		continue;

discard:
		kfree_skb(skb);
	}
	if (rpc)
		homa_rpc_unlock(rpc);

	while (num_acks > 0) {
		num_acks--;
		homa_rpc_acked(hsk, &saddr, &acks[num_acks]);
	}

	if (hsk->dead_skbs >= 2 * hsk->homa->dead_buffs_limit)
		/* We get here if neither homa_wait_for_message
		 * nor homa_timer can keep up with reaping dead
		 * RPCs. See reap.txt for details.
		 */

		homa_rpc_reap(hsk, hsk->homa->reap_limit);
}

/**
 * homa_data_pkt() - Handler for incoming DATA packets
 * @skb:     Incoming packet; size known to be large enough for the header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet.
 *           Must be locked by the caller.
 */
void homa_data_pkt(struct sk_buff *skb, struct homa_rpc *rpc)
{
	struct data_header *h = (struct data_header *)skb->data;

	if (rpc->state != RPC_INCOMING && homa_is_client(rpc->id)) {
		if (unlikely(rpc->state != RPC_OUTGOING))
			goto discard;
		rpc->state = RPC_INCOMING;
		if (homa_message_in_init(rpc, ntohl(h->message_length)) != 0)
			goto discard;
	} else if (rpc->state != RPC_INCOMING) {
		/* Must be server; note that homa_rpc_new_server already
		 * initialized msgin and allocated buffers.
		 */
		if (unlikely(rpc->msgin.length >= 0))
			goto discard;
	}

	if (rpc->msgin.num_bpages == 0)
		/* Drop packets that arrive when we can't allocate buffer
		 * space. If we keep them around, packet buffer usage can
		 * exceed available cache space, resulting in poor
		 * performance.
		 */
		goto discard;

	homa_add_packet(rpc, skb);

	if (skb_queue_len(&rpc->msgin.packets) != 0 &&
	    !(atomic_read(&rpc->flags) & RPC_PKTS_READY)) {
		atomic_or(RPC_PKTS_READY, &rpc->flags);
		homa_sock_lock(rpc->hsk, "homa_data_pkt");
		homa_rpc_handoff(rpc);
		homa_sock_unlock(rpc->hsk);
	}
	return;

discard:
	kfree_skb(skb);
	UNIT_LOG("; ", "homa_data_pkt discarded packet");
}

/**
 * homa_resend_pkt() - Handler for incoming RESEND packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet; must
 *           be locked by caller, but may be NULL if there is no RPC matching
 *           this packet
 * @hsk:     Socket on which the packet was received.
 */
void homa_resend_pkt(struct sk_buff *skb, struct homa_rpc *rpc,
		     struct homa_sock *hsk)
{
	struct resend_header *h = (struct resend_header *)skb->data;
	struct busy_header busy;

	if (!rpc) {
		homa_xmit_unknown(skb, hsk);
		goto done;
	}

	if (!homa_is_client(rpc->id) && rpc->state != RPC_OUTGOING) {
		/* We are the server for this RPC and don't yet have a
		 * response packet, so just send BUSY.
		 */
		homa_xmit_control(BUSY, &busy, sizeof(busy), rpc);
		goto done;
	}
	if (ntohl(h->length) == 0)
		/* This RESEND is from a server just trying to determine
		 * whether the client still cares about the RPC; return
		 * BUSY so the server doesn't time us out.
		 */
		homa_xmit_control(BUSY, &busy, sizeof(busy), rpc);
	homa_resend_data(rpc, ntohl(h->offset),
			 ntohl(h->offset) + ntohl(h->length));

done:
	kfree_skb(skb);
}

/**
 * homa_unknown_pkt() - Handler for incoming UNKNOWN packets.
 * @skb:     Incoming packet; size known to be large enough for the header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet.
 */
void homa_unknown_pkt(struct sk_buff *skb, struct homa_rpc *rpc)
{
	if (homa_is_client(rpc->id)) {
		if (rpc->state == RPC_OUTGOING) {
			/* It appears that everything we've already transmitted
			 * has been lost; retransmit it.
			 */
			homa_resend_data(rpc, 0, rpc->msgout.next_xmit_offset);
			goto done;
		}

	} else {
		homa_rpc_free(rpc);
	}
done:
	kfree_skb(skb);
}

/**
 * homa_need_ack_pkt() - Handler for incoming NEED_ACK packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @hsk:     Socket on which the packet was received.
 * @rpc:     The RPC named in the packet header, or NULL if no such
 *           RPC exists. The RPC has been locked by the caller.
 */
void homa_need_ack_pkt(struct sk_buff *skb, struct homa_sock *hsk,
		       struct homa_rpc *rpc)
{
	struct common_header *h = (struct common_header *)skb->data;
	const struct in6_addr saddr = skb_canonical_ipv6_saddr(skb);
	__u64 id = homa_local_id(h->sender_id);
	struct homa_peer *peer;
	struct ack_header ack;

	/* Return if it's not safe for the peer to purge its state
	 * for this RPC (the RPC still exists and we haven't received
	 * the entire response), or if we can't find peer info.
	 */
	if (rpc && (rpc->state != RPC_INCOMING ||
		    rpc->msgin.bytes_remaining)) {
		goto done;
	} else {
		peer = homa_peer_find(hsk->homa->peers, &saddr, &hsk->inet);
		if (IS_ERR(peer))
			goto done;
	}

	/* Send an ACK for this RPC. At the same time, include all of the
	 * other acks available for the peer. Note: can't use rpc below,
	 * since it may be NULL.
	 */
	ack.common.type = ACK;
	ack.common.sport = h->dport;
	ack.common.dport = h->sport;
	ack.common.sender_id = cpu_to_be64(id);
	ack.num_acks = htons(homa_peer_get_acks(peer,
						HOMA_MAX_ACKS_PER_PKT,
						ack.acks));
	__homa_xmit_control(&ack, sizeof(ack), peer, hsk);

done:
	kfree_skb(skb);
}

/**
 * homa_ack_pkt() - Handler for incoming ACK packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @hsk:     Socket on which the packet was received.
 * @rpc:     The RPC named in the packet header, or NULL if no such
 *           RPC exists. The RPC has been locked by the caller but will
 *           be unlocked here.
 */
void homa_ack_pkt(struct sk_buff *skb, struct homa_sock *hsk,
		  struct homa_rpc *rpc)
{
	const struct in6_addr saddr = skb_canonical_ipv6_saddr(skb);
	struct ack_header *h = (struct ack_header *)skb->data;
	int i, count;

	if (rpc) {
		homa_rpc_free(rpc);
		homa_rpc_unlock(rpc);
	}

	count = ntohs(h->num_acks);
	for (i = 0; i < count; i++)
		homa_rpc_acked(hsk, &saddr, &h->acks[i]);
	kfree_skb(skb);
}

/**
 * homa_rpc_abort() - Terminate an RPC.
 * @rpc:     RPC to be terminated.  Must be locked by caller.
 * @error:   A negative errno value indicating the error that caused the abort.
 *           If this is a client RPC, the error will be returned to the
 *           application; if it's a server RPC, the error is ignored and
 *           we just free the RPC.
 */
void homa_rpc_abort(struct homa_rpc *rpc, int error)
{
	if (!homa_is_client(rpc->id)) {
		homa_rpc_free(rpc);
		return;
	}
	rpc->error = error;
	homa_sock_lock(rpc->hsk, "homa_rpc_abort");
	if (!rpc->hsk->shutdown)
		homa_rpc_handoff(rpc);
	homa_sock_unlock(rpc->hsk);
}

/**
 * homa_abort_rpcs() - Abort all RPCs to/from a particular peer.
 * @homa:    Overall data about the Homa protocol implementation.
 * @addr:    Address (network order) of the destination whose RPCs are
 *           to be aborted.
 * @port:    If nonzero, then RPCs will only be aborted if they were
 *	     targeted at this server port.
 * @error:   Negative errno value indicating the reason for the abort.
 */
void homa_abort_rpcs(struct homa *homa, const struct in6_addr *addr,
		     int port, int error)
{
	struct homa_socktab_scan scan;
	struct homa_rpc *rpc, *tmp;
	struct homa_sock *hsk;

	rcu_read_lock();
	for (hsk = homa_socktab_start_scan(homa->port_map, &scan); hsk;
	     hsk = homa_socktab_next(&scan)) {
		/* Skip the (expensive) lock acquisition if there's no
		 * work to do.
		 */
		if (list_empty(&hsk->active_rpcs))
			continue;
		if (!homa_protect_rpcs(hsk))
			continue;
		list_for_each_entry_safe(rpc, tmp, &hsk->active_rpcs,
					 active_links) {
			if (!ipv6_addr_equal(&rpc->peer->addr, addr))
				continue;
			if (port && rpc->dport != port)
				continue;
			homa_rpc_lock(rpc, "rpc_abort_rpcs");
			homa_rpc_abort(rpc, error);
			homa_rpc_unlock(rpc);
		}
		homa_unprotect_rpcs(hsk);
	}
	rcu_read_unlock();
}

/**
 * homa_abort_sock_rpcs() - Abort all outgoing (client-side) RPCs on a given
 * socket.
 * @hsk:         Socket whose RPCs should be aborted.
 * @error:       Zero means that the aborted RPCs should be freed immediately.
 *               A nonzero value means that the RPCs should be marked
 *               complete, so that they can be returned to the application;
 *               this value (a negative errno) will be returned from
 *               recvmsg.
 */
void homa_abort_sock_rpcs(struct homa_sock *hsk, int error)
{
	struct homa_rpc *rpc, *tmp;

	rcu_read_lock();
	if (list_empty(&hsk->active_rpcs))
		goto done;
	if (!homa_protect_rpcs(hsk))
		goto done;
	list_for_each_entry_safe(rpc, tmp, &hsk->active_rpcs, active_links) {
		if (!homa_is_client(rpc->id))
			continue;
		homa_rpc_lock(rpc, "homa_abort_sock_rpcs");
		if (rpc->state == RPC_DEAD) {
			homa_rpc_unlock(rpc);
			continue;
		}
		if (error)
			homa_rpc_abort(rpc, error);
		else
			homa_rpc_free(rpc);
		homa_rpc_unlock(rpc);
	}
	homa_unprotect_rpcs(hsk);
done:
	rcu_read_unlock();
}

/**
 * homa_register_interests() - Records information in various places so
 * that a thread will be woken up if an RPC that it cares about becomes
 * available.
 * @interest:     Used to record information about the messages this thread is
 *                waiting on. The initial contents of the structure are
 *                assumed to be undefined.
 * @hsk:          Socket on which relevant messages will arrive.  Must not be
 *                locked.
 * @flags:        Flags field from homa_recvmsg_args; see manual entry for
 *                details.
 * @id:           If non-zero, then the caller is interested in receiving
 *                the response for this RPC (@id must be a client request).
 * Return:        Either zero or a negative errno value. If a matching RPC
 *                is already available, information about it will be stored in
 *                interest.
 */
int homa_register_interests(struct homa_interest *interest,
			    struct homa_sock *hsk, int flags, __u64 id)
{
	struct homa_rpc *rpc = NULL;

	homa_interest_init(interest);
	interest->locked = 1;
	if (id != 0) {
		if (!homa_is_client(id))
			return -EINVAL;
		rpc = homa_find_client_rpc(hsk, id);
		if (!rpc)
			return -EINVAL;
		if (rpc->interest && rpc->interest != interest) {
			homa_rpc_unlock(rpc);
			return -EINVAL;
		}
	}

	/* Need both the RPC lock (acquired above) and the socket lock to
	 * avoid races.
	 */
	homa_sock_lock(hsk, "homa_register_interests");
	if (hsk->shutdown) {
		homa_sock_unlock(hsk);
		if (rpc)
			homa_rpc_unlock(rpc);
		return -ESHUTDOWN;
	}

	if (id != 0) {
		if ((atomic_read(&rpc->flags) & RPC_PKTS_READY) || rpc->error)
			goto claim_rpc;
		rpc->interest = interest;
		interest->reg_rpc = rpc;
		homa_rpc_unlock(rpc);
	}

	interest->locked = 0;
	if (flags & HOMA_RECVMSG_RESPONSE) {
		if (!list_empty(&hsk->ready_responses)) {
			rpc = list_first_entry(&hsk->ready_responses,
					       struct homa_rpc,
					       ready_links);
			goto claim_rpc;
		}
		/* Insert this thread at the *front* of the list;
		 * we'll get better cache locality if we reuse
		 * the same thread over and over, rather than
		 * round-robining between threads.  Same below.
		 */
		list_add(&interest->response_links,
			 &hsk->response_interests);
	}
	if (flags & HOMA_RECVMSG_REQUEST) {
		if (!list_empty(&hsk->ready_requests)) {
			rpc = list_first_entry(&hsk->ready_requests,
					       struct homa_rpc, ready_links);
			/* Make sure the interest isn't on the response list;
			 * otherwise it might receive a second RPC.
			 */
			if (interest->response_links.next != LIST_POISON1)
				list_del(&interest->response_links);
			goto claim_rpc;
		}
		list_add(&interest->request_links, &hsk->request_interests);
	}
	homa_sock_unlock(hsk);
	return 0;

claim_rpc:
	list_del_init(&rpc->ready_links);
	if (!list_empty(&hsk->ready_requests) ||
	    !list_empty(&hsk->ready_responses)) {
		// There are still more RPCs available, so let Linux know.
		hsk->sock.sk_data_ready(&hsk->sock);
	}

	/* This flag is needed to keep the RPC from being reaped during the
	 * gap between when we release the socket lock and we acquire the
	 * RPC lock.
	 */
	atomic_or(RPC_HANDING_OFF, &rpc->flags);
	homa_sock_unlock(hsk);
	if (!interest->locked) {
		atomic_or(APP_NEEDS_LOCK, &rpc->flags);
		homa_rpc_lock(rpc, "homa_register_interests");
		atomic_andnot(APP_NEEDS_LOCK, &rpc->flags);
		interest->locked = 1;
	}
	atomic_andnot(RPC_HANDING_OFF, &rpc->flags);
	atomic_long_set_release(&interest->ready_rpc, (long)rpc);
	return 0;
}

/**
 * homa_wait_for_message() - Wait for receipt of an incoming message
 * that matches the parameters. Various other activities can occur while
 * waiting, such as reaping dead RPCs and copying data to user space.
 * @hsk:          Socket where messages will arrive.
 * @flags:        Flags field from homa_recvmsg_args; see manual entry for
 *                details.
 * @id:           If non-zero, then a response message matching this id may
 *                be returned (@id must refer to a client request).
 *
 * Return:   Pointer to an RPC that matches @flags and @id, or a negative
 *           errno value. The RPC will be locked; the caller must unlock.
 */
struct homa_rpc *homa_wait_for_message(struct homa_sock *hsk, int flags,
				       __u64 id)
{
	int error;
	struct homa_rpc *result = NULL;
	struct homa_interest interest;
	struct homa_rpc *rpc = NULL;

	/* Each iteration of this loop finds an RPC, but it might not be
	 * in a state where we can return it (e.g., there might be packets
	 * ready to transfer to user space, but the incoming message isn't yet
	 * complete). Thus it could take many iterations of this loop
	 * before we have an RPC with a complete message.
	 */
	while (1) {
		error = homa_register_interests(&interest, hsk, flags, id);
		rpc = (struct homa_rpc *)atomic_long_read(&interest.ready_rpc);
		if (rpc)
			goto found_rpc;
		if (error < 0) {
			result = ERR_PTR(error);
			goto found_rpc;
		}

//		tt_record3("Preparing to poll, socket %d, flags 0x%x, pid %d",
//				hsk->client_port, flags, current->pid);

		/* There is no ready RPC so far. Clean up dead RPCs before
		 * going to sleep (or returning, if in nonblocking mode).
		 */
		while (1) {
			int reaper_result;

			rpc = (struct homa_rpc *)atomic_long_read(&interest
								  .ready_rpc);
			if (rpc)
				goto found_rpc;
			reaper_result = homa_rpc_reap(hsk,
						      hsk->homa->reap_limit);
			if (reaper_result == 0)
				break;

			/* Give NAPI and SoftIRQ tasks a chance to run. */
			schedule();
		}
		if (flags & HOMA_RECVMSG_NONBLOCKING) {
			result = ERR_PTR(-EAGAIN);
			goto found_rpc;
		}

		/* Now it's time to sleep. */
		set_current_state(TASK_INTERRUPTIBLE);
		rpc = (struct homa_rpc *)atomic_long_read(&interest.ready_rpc);
		if (!rpc && !hsk->shutdown)
			schedule();
		__set_current_state(TASK_RUNNING);

found_rpc:
		/* If we get here, it means either an RPC is ready for our
		 * attention or an error occurred.
		 *
		 * First, clean up all of the interests. Must do this before
		 * making any other decisions, because until we do, an incoming
		 * message could still be passed to us. Note: if we went to
		 * sleep, then this info was already cleaned up by whoever
		 * woke us up. Also, values in the interest may change between
		 * when we test them below and when we acquire the socket lock,
		 * so they have to be checked again after locking the socket.
		 */
		UNIT_HOOK("found_rpc");
		if (interest.reg_rpc ||
		    interest.request_links.next != LIST_POISON1 ||
		    interest.response_links.next != LIST_POISON1) {
			homa_sock_lock(hsk, "homa_wait_for_message");
			if (interest.reg_rpc)
				interest.reg_rpc->interest = NULL;
			if (interest.request_links.next != LIST_POISON1)
				list_del(&interest.request_links);
			if (interest.response_links.next != LIST_POISON1)
				list_del(&interest.response_links);
			homa_sock_unlock(hsk);
		}

		/* Now check to see if we received an RPC handoff (note that
		 * this could have happened anytime up until we reset the
		 * interests above).
		 */
		rpc = (struct homa_rpc *)atomic_long_read(&interest.ready_rpc);
		if (rpc) {
			if (!interest.locked) {
				atomic_or(APP_NEEDS_LOCK, &rpc->flags);
				homa_rpc_lock(rpc, "homa_wait_for_message");
				atomic_andnot(APP_NEEDS_LOCK | RPC_HANDING_OFF,
					      &rpc->flags);
			} else {
				atomic_andnot(RPC_HANDING_OFF, &rpc->flags);
			}
			if (!rpc->error)
				rpc->error = homa_copy_to_user(rpc);
			if (rpc->state == RPC_DEAD) {
				homa_rpc_unlock(rpc);
				continue;
			}
			if (rpc->error)
				goto done;
			atomic_andnot(RPC_PKTS_READY, &rpc->flags);
			if (rpc->msgin.bytes_remaining == 0 &&
			    !skb_queue_len(&rpc->msgin.packets))
				goto done;
			homa_rpc_unlock(rpc);
		}

		/* A complete message isn't available: check for errors. */
		if (IS_ERR(result))
			return result;
		if (signal_pending(current))
			return ERR_PTR(-EINTR);

		/* No message and no error; try again. */
	}

done:
	return rpc;
}

/**
 * homa_choose_interest() - Given a list of interests for an incoming
 * message, choose the best one to handle it (if any).
 * @homa:        Overall information about the Homa transport.
 * @head:        Head pointers for the list of interest: either
 *		 hsk->request_interests or hsk->response_interests.
 * @offset:      Offset of "next" pointers in the list elements (either
 *               offsetof(request_links) or offsetof(response_links).
 * Return:       An interest to use for the incoming message, or NULL if none
 *               is available. If possible, this function tries to pick an
 *               interest whose thread is running on a core that isn't
 *               currently busy doing Homa transport work.
 */
struct homa_interest *homa_choose_interest(struct homa *homa,
					   struct list_head *head, int offset)
{
	struct homa_interest *backup = NULL;
	struct homa_interest *interest;
	struct list_head *pos;

	list_for_each(pos, head) {
		interest = (struct homa_interest *)(((char *)pos) - offset);
		if (!backup)
			backup = interest;
	}

	/* All interested threads are on busy cores; return the first. */
	return backup;
}

/**
 * homa_rpc_handoff() - This function is called when the input message for
 * an RPC is ready for attention from a user thread. It either notifies
 * a waiting reader or queues the RPC.
 * @rpc:                RPC to handoff; must be locked. The caller must
 *			also have locked the socket for this RPC.
 */
void homa_rpc_handoff(struct homa_rpc *rpc)
{
	struct homa_sock *hsk = rpc->hsk;
	struct homa_interest *interest;

	if ((atomic_read(&rpc->flags) & RPC_HANDING_OFF) ||
	    !list_empty(&rpc->ready_links))
		return;

	/* First, see if someone is interested in this RPC specifically.
	 */
	if (rpc->interest) {
		interest = rpc->interest;
		goto thread_waiting;
	}

	/* Second, check the interest list for this type of RPC. */
	if (homa_is_client(rpc->id)) {
		interest = homa_choose_interest(hsk->homa,
						&hsk->response_interests,
						offsetof(struct homa_interest,
							 response_links));
		if (interest)
			goto thread_waiting;
		list_add_tail(&rpc->ready_links, &hsk->ready_responses);
	} else {
		interest = homa_choose_interest(hsk->homa,
						&hsk->request_interests,
						offsetof(struct homa_interest,
							 request_links));
		if (interest)
			goto thread_waiting;
		list_add_tail(&rpc->ready_links, &hsk->ready_requests);
	}

	/* If we get here, no-one is waiting for the RPC, so it has been
	 * queued.
	 */

	/* Notify the poll mechanism. */
	hsk->sock.sk_data_ready(&hsk->sock);
	return;

thread_waiting:
	/* We found a waiting thread. The following 3 lines must be here,
	 * before clearing the interest, in order to avoid a race with
	 * homa_wait_for_message (which won't acquire the socket lock if
	 * the interest is clear).
	 */
	atomic_or(RPC_HANDING_OFF, &rpc->flags);
	interest->locked = 0;
	atomic_long_set_release(&interest->ready_rpc, (long)rpc);

	/* Clear the interest. This serves two purposes. First, it saves
	 * the waking thread from acquiring the socket lock again, which
	 * reduces contention on that lock). Second, it ensures that
	 * no-one else attempts to give this interest a different RPC.
	 */
	if (interest->reg_rpc) {
		interest->reg_rpc->interest = NULL;
		interest->reg_rpc = NULL;
	}
	if (interest->request_links.next != LIST_POISON1)
		list_del(&interest->request_links);
	if (interest->response_links.next != LIST_POISON1)
		list_del(&interest->response_links);
	wake_up_process(interest->thread);
}

/**
 * homa_incoming_sysctl_changed() - Invoked whenever a sysctl value is changed;
 * any input-related parameters that depend on sysctl-settable values.
 * @homa:    Overall data about the Homa protocol implementation.
 */
void homa_incoming_sysctl_changed(struct homa *homa)
{
	__u64 tmp;

	/* Code below is written carefully to avoid integer underflow or
	 * overflow under expected usage patterns. Be careful when changing!
	 */

	tmp = homa->busy_usecs;
	tmp = (tmp * cpu_khz) / 1000;
	homa->busy_cycles = tmp;

	tmp = homa->gro_busy_usecs;
	tmp = (tmp * cpu_khz) / 1000;
	homa->gro_busy_cycles = tmp;

	tmp = homa->bpage_lease_usecs;
	tmp = (tmp * cpu_khz) / 1000;
	homa->bpage_lease_cycles = tmp;
}
