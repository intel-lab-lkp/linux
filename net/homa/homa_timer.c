// SPDX-License-Identifier: BSD-2-Clause

/* This file handles timing-related functions for Homa, such as retries
 * and timeouts.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_rpc.h"

/**
 * homa_check_rpc() -  Invoked for each RPC during each timer pass; does
 * most of the work of checking for time-related actions such as sending
 * resends, aborting RPCs for which there is no response, and sending
 * requests for acks. It is separate from homa_timer because homa_timer
 * got too long and deeply indented.
 * @rpc:     RPC to check; must be locked by the caller.
 */
void homa_check_rpc(struct homa_rpc *rpc)
{
	struct homa *homa = rpc->hsk->homa;
	struct resend_header resend;

	/* See if we need to request an ack for this RPC. */
	if (!homa_is_client(rpc->id) && rpc->state == RPC_OUTGOING &&
	    rpc->msgout.next_xmit_offset >= rpc->msgout.length) {
		if (rpc->done_timer_ticks == 0) {
			rpc->done_timer_ticks = homa->timer_ticks;
		} else {
			/* >= comparison that handles tick wrap-around. */
			if ((rpc->done_timer_ticks + homa->request_ack_ticks
					- 1 - homa->timer_ticks) & 1 << 31) {
				struct need_ack_header h;

				homa_xmit_control(NEED_ACK, &h, sizeof(h), rpc);
			}
		}
	}

	if (rpc->state == RPC_INCOMING) {
		if (rpc->msgin.num_bpages == 0) {
			/* Waiting for buffer space, so no problem. */
			rpc->silent_ticks = 0;
			return;
		}
	} else if (!homa_is_client(rpc->id)) {
		/* We're the server and we've received the input message;
		 * no need to worry about retries.
		 */
		rpc->silent_ticks = 0;
		return;
	}

	if (rpc->state == RPC_OUTGOING) {
		if (rpc->msgout.next_xmit_offset < rpc->msgout.length) {
			/* There are bytes that we haven't transmitted,
			 * so no need to be concerned; the ball is in our court.
			 */
			rpc->silent_ticks = 0;
			return;
		}
	}

	if (rpc->silent_ticks < homa->resend_ticks)
		return;
	if (rpc->silent_ticks >= homa->timeout_ticks) {
		homa_rpc_abort(rpc, -ETIMEDOUT);
		return;
	}
	if (((rpc->silent_ticks - homa->resend_ticks) % homa->resend_interval)
			!= 0)
		return;

	/* Issue a resend for the bytes just after the last ones received
	 * (gaps in the middle were already handled by homa_gap_retry above).
	 */
	if (rpc->msgin.length < 0) {
		/* Haven't received any data for this message; request
		 * retransmission of just the first packet (the sender
		 * will send at least one full packet, regardless of
		 * the length below).
		 */
		resend.offset = htonl(0);
		resend.length = htonl(100);
	} else {
		homa_gap_retry(rpc);
		resend.offset = htonl(rpc->msgin.recv_end);
		resend.length = htonl(rpc->msgin.length - rpc->msgin.recv_end);
		if (resend.length == 0)
			return;
	}
	homa_xmit_control(RESEND, &resend, sizeof(resend), rpc);
}

/**
 * homa_timer() - This function is invoked at regular intervals ("ticks")
 * to implement retries and aborts for Homa.
 * @homa:    Overall data about the Homa protocol implementation.
 */
void homa_timer(struct homa *homa)
{
	struct homa_socktab_scan scan;
	struct homa_sock *hsk;
	struct homa_rpc *rpc;
	int total_rpcs = 0;
	int rpc_count = 0;

	homa->timer_ticks++;

	/* Scan all existing RPCs in all sockets.  The rcu_read_lock
	 * below prevents sockets from being deleted during the scan.
	 */
	rcu_read_lock();
	for (hsk = homa_socktab_start_scan(homa->port_map, &scan);
			hsk; hsk = homa_socktab_next(&scan)) {
		while (hsk->dead_skbs >= homa->dead_buffs_limit)
			/* If we get here, it means that homa_wait_for_message
			 * isn't keeping up with RPC reaping, so we'll help
			 * out.  See reap.txt for more info.
			 */
			if (homa_rpc_reap(hsk, hsk->homa->reap_limit) == 0)
				break;

		if (list_empty(&hsk->active_rpcs) || hsk->shutdown)
			continue;

		if (!homa_protect_rpcs(hsk))
			continue;
		list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links) {
			total_rpcs++;
			homa_rpc_lock(rpc, "homa_timer");
			if (rpc->state == RPC_IN_SERVICE) {
				rpc->silent_ticks = 0;
				homa_rpc_unlock(rpc);
				continue;
			}
			rpc->silent_ticks++;
			homa_check_rpc(rpc);
			homa_rpc_unlock(rpc);
			rpc_count++;
			if (rpc_count >= 10) {
				/* Give other kernel threads a chance to run
				 * on this core. Must release the RCU read lock
				 * while doing this.
				 */
				rcu_read_unlock();
				schedule();
				rcu_read_lock();
				rpc_count = 0;
			}
		}
		homa_unprotect_rpcs(hsk);
	}
	rcu_read_unlock();

//	if (total_rpcs > 0)
//		tt_record1("homa_timer finished scanning %d RPCs", total_rpcs);
}
