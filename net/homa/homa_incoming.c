// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0+

/* This file contains functions that handle incoming Homa messages. */

#include "homa_impl.h"
#include "homa_interest.h"
#include "homa_peer.h"
#include "homa_pool.h"

/**
 * homa_message_in_init() - Constructor for homa_message_in.
 * @rpc:          RPC whose msgin structure should be initialized. The
 *                msgin struct is assumed to be zeroes.
 * @length:       Total number of bytes in message.
 * Return:        Zero for successful initialization, or a negative errno
 *                if rpc->msgin could not be initialized.
 */
int homa_message_in_init(struct homa_rpc *rpc, int length)
	__must_hold(rpc->bucket->lock)
{
	int err;

	if (((unsigned int)length) > HOMA_MAX_MESSAGE_LENGTH)
		return -EINVAL;

	rpc->msgin.length = length;
	__skb_queue_head_init(&rpc->msgin.packets);
	INIT_LIST_HEAD(&rpc->msgin.gaps);
	rpc->msgin.bytes_remaining = length;
	err = homa_pool_alloc_msg(rpc);
	if (err != 0) {
		rpc->msgin.length = -1;
		return err;
	}
	return 0;
}

/**
 * homa_gap_alloc() - Allocate a new gap and add it to a gap list.
 * @next:   Add the new gap just before this list element.
 * @start:  Offset of first byte covered by the gap.
 * @end:    Offset of byte just after the last one covered by the gap.
 * Return:  Pointer to the new gap, or NULL if memory couldn't be allocated
 *          for the gap object.
 */
struct homa_gap *homa_gap_alloc(struct list_head *next, int start, int end)
{
	struct homa_gap *gap;

	gap = kmalloc_obj(*gap, GFP_ATOMIC);
	if (!gap)
		return NULL;
	gap->start = start;
	gap->end = end;
	gap->time = homa_clock();
	list_add_tail(&gap->links, next);
	return gap;
}

/**
 * homa_request_retrans() - The function is invoked when it appears that
 * data packets for a message have been lost. It issues RESEND requests
 * as appropriate and may modify the state of the RPC.
 * @rpc:     RPC for which incoming data is delinquent; must be locked by
 *           caller.
 */
void homa_request_retrans(struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	struct homa_resend_hdr *resends;
	struct homa_resend_hdr resend;
	struct homa_gap *gap;
	int offset, length;
	int num_gaps, i;

	if (rpc->msgin.length >= 0) {
		/* Issue RESENDS for any gaps in incoming data.  Must
		 * pre-allocate space for all of the packet headers because
		 * so we don't have to release the RPC lock while iterating
		 * the gap list.
		 */
		num_gaps = list_count_nodes(&rpc->msgin.gaps);
		resends = kzalloc_objs(*resends, num_gaps, GFP_ATOMIC);
		if (!resends)
			return;
		i = 0;
		list_for_each_entry(gap, &rpc->msgin.gaps, links) {
			resends[i].offset = htonl(gap->start);
			resends[i].length = htonl(gap->end - gap->start);
			i++;
		}
		for (i = 0; i < num_gaps; i++)
			homa_xmit_control(RESEND, &resends[i],
					  sizeof(resends[i]), rpc);
		kfree(resends);

		/* Issue a RESEND for any granted data after the last gap. */
		offset = rpc->msgin.recv_end;
		length = rpc->msgin.length - rpc->msgin.recv_end;
		if (length <= 0)
			return;
	} else {
		/* No data has been received for the RPC. Ask the sender to
		 * resend everything it has sent so far.
		 */
		offset = 0;
		length = -1;
	}

	memset(&resend, 0, sizeof(resend));
	resend.offset = htonl(offset);
	resend.length = htonl(length);
	homa_xmit_control(RESEND, &resend, sizeof(resend), rpc);
}

/**
 * homa_add_packet() - Add an incoming packet to the contents of a
 * partially received message.
 * @rpc:   Add the packet to the msgin for this RPC.
 * @skb:   The new packet. If the packet is linked into rpc then an additional
 *         reference will be taken on it.
 * Return: 0 if the packet was successfully linked into rpc, otherwise the
 *         reason why the packet was not linked in.
 */
enum skb_drop_reason homa_add_packet(struct homa_rpc *rpc, struct sk_buff *skb)
	__must_hold(rpc->bucket->lock)
{
	struct homa_data_hdr *h = (struct homa_data_hdr *)skb->data;
	struct homa_gap *gap, *dummy, *gap2;
	u32 start = ntohl(h->seg.offset);
	u32 length = homa_data_len(skb);
	enum skb_drop_reason reason = 0;
	u32 end = start + length;

	if (start >= rpc->msgin.length ||
	    length > (rpc->msgin.length - start)) {
		reason = SKB_DROP_REASON_PKT_TOO_BIG;
		goto ignore;
	}

	if (start == rpc->msgin.recv_end) {
		/* Common case: packet is sequential. */
		rpc->msgin.recv_end += length;
		goto keep;
	}

	if (start > rpc->msgin.recv_end) {
		/* Packet creates a new gap. */
		if (!homa_gap_alloc(&rpc->msgin.gaps,
				    rpc->msgin.recv_end, start)) {
			reason = SKB_DROP_REASON_NOMEM;
			goto ignore;
		}
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
			if (start < gap->start) {
				reason = SKB_DROP_REASON_DUP_FRAG;
				goto ignore;
			}
			if (end > gap->end) {
				reason = SKB_DROP_REASON_DUP_FRAG;
				goto ignore;
			}
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
			if (end > gap->end) {
				reason = SKB_DROP_REASON_DUP_FRAG;
				goto ignore;
			}
			gap->end = start;
			goto keep;
		}

		/* Packet is in the middle of the gap; must split the gap. */
		gap2 = homa_gap_alloc(&gap->links, gap->start, start);
		if (!gap2) {
			reason = SKB_DROP_REASON_NOMEM;
			goto ignore;
		}
		gap2->time = gap->time;
		gap->start = end;
		goto keep;
	}
	/* Packet doesn't overlap any gap, so it is a duplicate. */
	reason = SKB_DROP_REASON_DUP_FRAG;

ignore:
	return reason;

keep:
	__skb_queue_tail(&rpc->msgin.packets, skb);
	skb_get(skb);
	rpc->msgin.bytes_remaining -= length;
	return 0;
}

/**
 * homa_copy_to_user() - Copy as much data as possible from incoming
 * packet buffers to buffers in user space.
 * @rpc:     RPC for which data should be copied. Must be locked by caller.
 * Return:   Zero for success or a negative errno if there is an error.
 *           It is possible for the RPC to be freed while this function
 *           executes (it releases and reacquires the RPC lock). If that
 *           happens, -EINVAL will be returned and the state of @rpc
 *           will be RPC_DEAD. Clears the RPC_PKTS_READY bit in @rpc->flags
 *           if all available packets have been copied out.
 */
int homa_copy_to_user(struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
#define MAX_SKBS 20
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
		if (n == 0) {
			clear_bit(RPC_PKTS_READY, &rpc->flags);
			break;
		}

		/* At this point we've collected a batch of packets (or
		 * run out of packets); copy any available packets out to
		 * user space.
		 */
		homa_rpc_unlock(rpc);

		/* Each iteration of this loop copies out one skb. */
		for (i = 0; i < n; i++) {
			struct homa_data_hdr *h = (struct homa_data_hdr *)
					skbs[i]->data;
			int pkt_length = homa_data_len(skbs[i]);
			int offset = ntohl(h->seg.offset);
			int buf_bytes, chunk_size;
			struct iov_iter iter;
			int copied = 0;
			char __user *dst;

			/* Each iteration of this loop copies to one
			 * user buffer.
			 */
			while (copied < pkt_length) {
				chunk_size = pkt_length - copied;
				dst = homa_pool_get_buffer(rpc, offset + copied,
							   &buf_bytes);
				if (buf_bytes < chunk_size) {
					if (buf_bytes == 0) {
						/* skb has data beyond message
						 * end?
						 */
						break;
					}
					chunk_size = buf_bytes;
				}
				error = import_ubuf(READ, dst, chunk_size,
						    &iter);
				if (error)
					goto free_skbs;
				error = skb_copy_datagram_iter(skbs[i],
							       sizeof(*h) +
							       copied,  &iter,
							       chunk_size);
				if (error)
					goto free_skbs;
				copied += chunk_size;
			}
		}

free_skbs:
		for (i = 0; i < n; i++)
			consume_skb(skbs[i]);
		n = 0;
		homa_rpc_lock_preempt(rpc);
		if (error)
			break;
	}
	return error;
}

/**
 * homa_dispatch_pkts() - Top-level function that processes a batch of packets,
 * all related to the same RPC.
 * @skb:       First packet in the batch, linked through skb->next.
 */
void homa_dispatch_pkts(struct sk_buff *skb)
{
	const struct in6_addr saddr = skb_canonical_ipv6_saddr(skb);
	struct homa_data_hdr *h = (struct homa_data_hdr *)skb->data;
	u64 id = homa_local_id(h->common.sender_id);
	int dport = ntohs(h->common.dport);
	struct homa_rpc *rpc = NULL;
	struct homa_sock *hsk;
	struct homa_net *hnet;
	struct sk_buff *next;

	/* Find the appropriate socket.*/
	hnet = homa_net(dev_net(skb->dev));
	hsk = homa_sock_find(hnet, dport);
	if (!hsk || (!homa_is_client(id) && !hsk->is_server)) {
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
		if (hsk)
			sock_put(&hsk->sock);
		return;
	}

	/* Each iteration through the following loop processes one packet. */
	for (; skb; skb = next) {
		h = (struct homa_data_hdr *)skb->data;
		next = skb->next;

		/* Relinquish the RPC lock temporarily if it's needed
		 * elsewhere.
		 */
		if (rpc) {
			if (test_bit(APP_NEEDS_LOCK, &rpc->flags)) {
				homa_rpc_unlock(rpc);

				/* This short spin is needed to ensure that the
				 * other thread gets the lock before this thread
				 * grabs it again below (the need for this
				 * was confirmed experimentally in 2/2025;
				 * without it, the handoff fails 20-25% of the
				 * time). Furthermore, the call to homa_spin
				 * seems to allow the other thread to acquire
				 * the lock more quickly.
				 */
				homa_spin(100);
				homa_rpc_lock(rpc);
			}
		}

		/* If we don't already have an RPC, find it, lock it,
		 * and create a reference on it.
		 */
		if (!rpc) {
			if (!homa_is_client(id)) {
				/* We are the server for this RPC. */
				if (h->common.type == DATA) {
					/* Create a new RPC if one doesn't
					 * already exist.
					 */
					rpc = homa_rpc_alloc_server(hsk, &saddr,
								    h);
					if (IS_ERR(rpc)) {
						rpc = NULL;
						goto discard;
					}
				} else {
					rpc = homa_rpc_find_server(hsk, &saddr,
								   id);
				}
			} else {
				rpc = homa_rpc_find_client(hsk, id);
			}
			if (rpc)
				homa_rpc_hold(rpc);
		}
		if (unlikely(!rpc)) {
			if (h->common.type != NEED_ACK &&
			    h->common.type != ACK &&
			    h->common.type != RESEND)
				goto discard;
		} else {
			if (h->common.type == DATA ||
			    h->common.type == BUSY)
				rpc->silent_ticks = 0;
		}

		switch (h->common.type) {
		case DATA:
			homa_data_pkt(skb, rpc);
			break;
		case RESEND:
			homa_resend_pkt(skb, rpc, hsk);
			break;
		case RPC_UNKNOWN:
			homa_rpc_unknown_pkt(skb, rpc);
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
			break;
		default:
			goto discard;
		}
		continue;

discard:
		kfree_skb(skb);
	}
	if (rpc) {
		homa_rpc_put(rpc);
		homa_rpc_unlock(rpc);
	}

	/* We need to reap dead RPCs here under two conditions:
	 * 1. The socket has hit its limit on tx buffer space and threads are
	 *    blocked waiting for skbs to be released.
	 * 2. A large number of dead RPCs have accumulated, and it seems
	 *    that the reaper isn't keeping up when invoked only at
	 *    "convenient" times (see "RPC Reaping Strategy" in homa_rpc_reap
	 *    code for details).
	 */
	if (hsk->dead_frags > 0) {
		while (test_bit(HOMA_SOCK_NOSPACE, &hsk->flags) ||
		       hsk->dead_frags >= 2 * hsk->homa->dead_frags_limit) {
			int more;

			more = homa_rpc_reap(hsk);
			if (!more)
				break;
		}
	}
	sock_put(&hsk->sock);
}

/**
 * homa_data_pkt() - Handler for incoming DATA packets
 * @skb:     Incoming packet; size known to be large enough for the header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet.
 *           Must be locked by the caller.
 */
void homa_data_pkt(struct sk_buff *skb, struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	enum skb_drop_reason drop_reason = SKB_DROP_REASON_NOT_SPECIFIED;
	struct homa_data_hdr *h = (struct homa_data_hdr *)skb->data;

	if (rpc->state != RPC_INCOMING && homa_is_client(rpc->id)) {
		if (unlikely(rpc->state != RPC_OUTGOING))
			goto handle_ack;
		rpc->state = RPC_INCOMING;
		if (homa_message_in_init(rpc, ntohl(h->message_length)) != 0) {
			drop_reason = SKB_DROP_REASON_NOMEM;
			goto handle_ack;
		}
	} else if (rpc->state != RPC_INCOMING) {
		/* Must be server; note that homa_rpc_alloc_server already
		 * initialized msgin and allocated buffers.
		 */
		if (unlikely(rpc->msgin.length >= 0))
			goto handle_ack;
	}

	if (rpc->msgin.num_bpages == 0) {
		/* Drop packets that arrive when we can't allocate buffer
		 * space. If we keep them around, packet buffer usage can
		 * exceed available cache space, resulting in poor
		 * performance.
		 */
		drop_reason = SKB_DROP_REASON_NOMEM;
		goto handle_ack;
	}

	drop_reason = homa_add_packet(rpc, skb);

	if (skb_queue_len(&rpc->msgin.packets) != 0 &&
	    !test_bit(RPC_PKTS_READY, &rpc->flags)) {
		set_bit(RPC_PKTS_READY, &rpc->flags);
		homa_rpc_handoff(rpc);
	}

handle_ack:
	/* It's important to handle acks *after* calling homa_add_packet.
	 * Otherwise we can end up with extraneous handoffs: we have to release
	 * the RPC lock to handle acks, but that could allow some other thread
	 * to receive a previous handoff made by homa_rpc_alloc_server, even
	 * though the packet hasn't been added to the RPC. If this happens
	 * the call to homa_rpc_handoff above will have to make an additional
	 * handoff.
	 */
	if (h->ack.client_id) {
		const struct in6_addr saddr = skb_canonical_ipv6_saddr(skb);

		homa_rpc_ack(rpc->hsk, rpc, &saddr, 1, &h->ack);
	}
	kfree_skb_reason(skb, drop_reason);
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
	__must_hold(rpc->bucket->lock)
{
	struct homa_resend_hdr *h = (struct homa_resend_hdr *)skb->data;
	int offset = ntohl(h->offset);
	int length = ntohl(h->length);
	int end = offset + length;
	struct homa_busy_hdr busy;
	int tx_end;

	if (!rpc) {
		homa_xmit_unknown(skb, hsk);
		goto done;
	}

	tx_end = homa_rpc_tx_end(rpc);
	if (!homa_is_client(rpc->id) && rpc->state != RPC_OUTGOING) {
		/* We are the server for this RPC and don't yet have a
		 * response message, so send BUSY to keep the client
		 * waiting.
		 */
		homa_xmit_control(BUSY, &busy, sizeof(busy), rpc);
		goto done;
	}

	if (length == -1)
		end = tx_end;

	/* Don't retransmit data that we haven't transmitted for the first
	 * time: we're not ready to send that data yet (we'll send a BUSY
	 * packet below if needed).
	 */
	homa_resend_data(rpc, offset, (end > tx_end) ? tx_end : end);
	if (rpc->state == RPC_DEAD)
		goto done;

	if (offset >= tx_end) {
		/* We have chosen not to transmit any of the requested data;
		 * send BUSY so the receiver knows we are alive.
		 */
		homa_xmit_control(BUSY, &busy, sizeof(busy), rpc);
		goto done;
	}

done:
	consume_skb(skb);
}

/**
 * homa_rpc_unknown_pkt() - Handler for incoming RPC_UNKNOWN packets.
 * @skb:     Incoming packet; size known to be large enough for the header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet. Must
 *           be locked by caller.
 */
void homa_rpc_unknown_pkt(struct sk_buff *skb, struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	if (homa_is_client(rpc->id)) {
		if (rpc->state == RPC_OUTGOING) {
			int tx_end = homa_rpc_tx_end(rpc);

			/* It appears that everything we've already transmitted
			 * has been lost; retransmit it.
			 */
			homa_resend_data(rpc, 0, tx_end);
			goto done;
		}
	} else {
		homa_rpc_end(rpc);
	}
done:
	consume_skb(skb);
}

/**
 * homa_need_ack_pkt() - Handler for incoming NEED_ACK packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @hsk:     Socket on which the packet was received.
 * @rpc:     The RPC named in the packet header, or NULL if no such
 *           RPC exists. The RPC must have been locked by the caller.
 */
void homa_need_ack_pkt(struct sk_buff *skb, struct homa_sock *hsk,
		       struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	struct homa_common_hdr *h = (struct homa_common_hdr *)skb->data;
	const struct in6_addr saddr = skb_canonical_ipv6_saddr(skb);
	u64 id = homa_local_id(h->sender_id);
	struct homa_route *route;
	struct homa_ack_hdr ack;

	/* Don't ack if it's not safe for the peer to purge its state
	 * for this RPC (the RPC still exists and we haven't received
	 * the entire response), or if we can't find peer info.
	 */
	if (rpc && (rpc->state != RPC_INCOMING ||
		    rpc->msgin.bytes_remaining)) {
		homa_request_retrans(rpc);
		goto done;
	}
	route = homa_route_get(hsk, &saddr);
	if (IS_ERR(route))
		goto done;

	/* Send an ACK for this RPC. At the same time, include all of the
	 * other acks available for the peer. Note: can't use rpc below,
	 * since it may be NULL.
	 */
	memset(&ack, 0, sizeof(ack));
	ack.common.type = ACK;
	ack.common.sport = h->dport;
	ack.common.dport = h->sport;
	ack.common.sender_id = cpu_to_be64(id);
	ack.num_acks = htons(homa_peer_get_acks(route->peer,
						HOMA_MAX_ACKS_PER_PKT,
						ack.acks));
	if (rpc)
		homa_rpc_unlock(rpc);
	__homa_xmit_control(&ack, sizeof(ack), route, hsk);
	if (rpc)
		homa_rpc_lock(rpc);
	homa_route_release(route);

done:
	consume_skb(skb);
}

/**
 * homa_ack_pkt() - Handler for incoming ACK packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @hsk:     Socket on which the packet was received.
 * @rpc:     The RPC named in the packet header, or NULL if no such
 *           RPC exists. The RPC must be locked by the caller.
 */
void homa_ack_pkt(struct sk_buff *skb, struct homa_sock *hsk,
		  struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	const struct in6_addr saddr = skb_canonical_ipv6_saddr(skb);
	struct homa_ack_hdr *h = (struct homa_ack_hdr *)skb->data;
	int count;

	if (rpc)
		homa_rpc_end(rpc);

	count = ntohs(h->num_acks);
	if (count > HOMA_MAX_ACKS_PER_PKT)
		count = HOMA_MAX_ACKS_PER_PKT;
	if (count > 0)
		homa_rpc_ack(hsk, rpc, &saddr, count, h->acks);
	consume_skb(skb);
}

/**
 * homa_wait_private() - Waits until the response has been received for
 * a specific RPC or the RPC has failed with an error.
 * @rpc:          RPC to wait for; an error will be returned if the RPC is
 *                not a client RPC or not private. Must be locked by caller.
 * @nonblocking:  Nonzero means return immediately if @rpc not ready.
 * Return:        0 means that @rpc is ready for attention: either its response
 *                has been received or it has an unrecoverable error such as
 *                ETIMEDOUT (in rpc->error). Nonzero means some other error
 *                (such as EINTR or EINVAL) occurred before @rpc became ready
 *                for attention; in this case the return value is a negative
 *                errno.
 */
int homa_wait_private(struct homa_rpc *rpc, int nonblocking)
	__must_hold(rpc->bucket->lock)
{
	struct homa_interest interest;
	int result;

	if (!test_bit(RPC_PRIVATE, &rpc->flags))
		return -EINVAL;

	/* Each iteration through this loop waits until rpc needs attention
	 * in some way (e.g. packets have arrived), then deals with that need
	 * (e.g. copy to user space). It may take many iterations until the
	 * RPC is ready for the application.
	 */
	while (1) {
		result = 0;
		if (!rpc->error)
			rpc->error = homa_copy_to_user(rpc);
		if (rpc->error)
			break;
		if (rpc->msgin.length >= 0 &&
		    rpc->msgin.bytes_remaining == 0 &&
		    skb_queue_len(&rpc->msgin.packets) == 0)
			break;

		if (nonblocking) {
			result = -EAGAIN;
			break;
		}

		result = homa_interest_init_private(&interest, rpc);
		if (result != 0)
			break;

		homa_rpc_unlock(rpc);
		result = homa_interest_wait(&interest);

		homa_rpc_lock_preempt(rpc);
		homa_interest_unlink_private(&interest);
		if (result != 0)
			break;
	}

	return result;
}

/**
 * homa_wait_shared() - Wait for the completion of any non-private
 * incoming message on a socket.
 * @hsk:          Socket on which to wait. Must not be locked.
 * @nonblocking:  Nonzero means return immediately if no RPC is ready.
 *
 * Return:    Pointer to an RPC with a complete incoming message or nonzero
 *            error field, or a negative errno (usually -EINTR). If an RPC
 *            is returned it will be locked and referenced; the caller
 *            must release the lock and the reference.
 */
struct homa_rpc *homa_wait_shared(struct homa_sock *hsk, int nonblocking)
	__cond_acquires(nonnull, rpc->bucket->lock)
{
	struct homa_interest interest;
	struct homa_rpc *rpc;
	int result;

	/* Each iteration through this loop waits until an RPC needs attention
	 * in some way (e.g. packets have arrived), then deals with that need
	 * (e.g. copy to user space). It may take many iterations until an
	 * RPC is ready for the application.
	 */
	while (1) {
		homa_sock_lock(hsk);
		if (hsk->shutdown) {
			rpc = ERR_PTR(-ESHUTDOWN);
			homa_sock_unlock(hsk);
			goto done;
		}
		if (!list_empty(&hsk->ready_rpcs)) {
			rpc = list_first_entry(&hsk->ready_rpcs,
					       struct homa_rpc,
					       ready_links);
			homa_rpc_hold(rpc);
			list_del_init(&rpc->ready_links);
			if (!list_empty(&hsk->ready_rpcs)) {
				/* There are still more RPCs available, so
				 * let Linux know.
				 */
				hsk->sock.sk_data_ready(&hsk->sock);
			}
			homa_sock_unlock(hsk);
		} else if (nonblocking) {
			rpc = ERR_PTR(-EAGAIN);
			homa_sock_unlock(hsk);

			/* This is a good time to cleanup dead RPCS. */
			homa_rpc_reap(hsk);
			goto done;
		} else {
			homa_interest_init_shared(&interest, hsk);
			homa_sock_unlock(hsk);
			result = homa_interest_wait(&interest);

			if (result != 0) {
				rpc = ERR_PTR(result);
				goto done;
			}

			rpc = interest.rpc;
			if (!rpc) {
				rpc = ERR_PTR(-ESHUTDOWN);
				goto done;
			}
		}

		homa_rpc_lock_preempt(rpc);
		if (!rpc->error)
			rpc->error = homa_copy_to_user(rpc);
		if (rpc->error) {
			if (rpc->state != RPC_DEAD)
				break;
		} else if (rpc->msgin.bytes_remaining == 0 &&
		    skb_queue_len(&rpc->msgin.packets) == 0)
			break;
		homa_rpc_put(rpc);
		homa_rpc_unlock(rpc);
	}

done:
	return rpc;
}

/**
 * homa_rpc_handoff() - This function is called when the input message for
 * an RPC is ready for attention from a user thread. It notifies a waiting
 * reader and/or queues the RPC, as appropriate.
 * @rpc:                RPC to handoff; must be locked.
 */
void homa_rpc_handoff(struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	struct homa_sock *hsk = rpc->hsk;

	if (rpc->state == RPC_DEAD)
		return;

	if (test_bit(RPC_PRIVATE, &rpc->flags)) {
		homa_interest_notify_private(rpc);
		return;
	}

	/* Shared RPC; if there is a waiting thread, hand off the RPC;
	 * otherwise enqueue it.
	 */
	homa_sock_lock(hsk);
	if (hsk->shutdown) {
		homa_sock_unlock(hsk);
		return;
	}
	if (!list_empty(&hsk->interests)) {
		homa_rpc_hold(rpc);
		homa_interest_notify_shared(hsk, rpc);
	} else if (list_empty(&rpc->ready_links)) {
		list_add_tail(&rpc->ready_links, &hsk->ready_rpcs);
		hsk->sock.sk_data_ready(&hsk->sock);
	}
	homa_sock_unlock(hsk);
}
