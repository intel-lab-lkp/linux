// SPDX-License-Identifier: BSD-2-Clause or GPL-2.0+

/* This file handles timing-related functions for Homa, such as retries
 * and timeouts.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_rpc.h"
#include "homa_stub.h"

/**
 * homa_timer_check_rpc() -  Invoked for each RPC during each timer pass; does
 * most of the work of checking for time-related actions such as sending
 * resends, aborting RPCs for which there is no response, and sending
 * requests for acks. It is separate from homa_timer because homa_timer
 * got too long and deeply indented.
 * @rpc:     RPC to check; must be locked by the caller.
 */
void homa_timer_check_rpc(struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	struct homa *homa = rpc->hsk->homa;
	int tx_end = homa_rpc_tx_end(rpc);

	/* See if we need to request an ack for this RPC. */
	if (!homa_is_client(rpc->id) && rpc->state == RPC_OUTGOING &&
	    tx_end == rpc->msgout.length) {
		if (rpc->done_timer_ticks == 0) {
			rpc->done_timer_ticks = homa->timer_ticks;
		} else {
			/* >= comparison that handles tick wrap-around. */
			if ((rpc->done_timer_ticks + homa->request_ack_ticks
					- 1 - homa->timer_ticks) & 1 << 31) {
				struct homa_need_ack_hdr h;

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
		if (tx_end < rpc->msgout.length) {
			/* There are granted bytes that we haven't transmitted,
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
			== 0)
		homa_request_retrans(rpc);
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
	int rpc_count = 0;

	homa->timer_ticks++;

	/* Scan all existing RPCs in all sockets. */
	for (hsk = homa_socktab_start_scan(homa->socktab, &scan);
			hsk; hsk = homa_socktab_next(&scan)) {
		while (hsk->dead_skbs >= homa->dead_buffs_limit)
			/* If we get here, it means that Homa isn't keeping
			 * up with RPC reaping, so we'll help out.  See
			 * "RPC Reaping Strategy" in homa_rpc_reap code for
			 * details.
			 */
			if (homa_rpc_reap(hsk, false) == 0)
				break;

		if (list_empty(&hsk->active_rpcs) || hsk->shutdown)
			continue;

		if (!homa_protect_rpcs(hsk))
			continue;
		rcu_read_lock();
		list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links) {
			homa_rpc_lock(rpc);
			if (rpc->state == RPC_IN_SERVICE) {
				rpc->silent_ticks = 0;
				homa_rpc_unlock(rpc);
				continue;
			}
			rpc->silent_ticks++;
			homa_timer_check_rpc(rpc);
			homa_rpc_unlock(rpc);
			rpc_count++;
			if (rpc_count >= 10) {
				/* Give other kernel threads a chance to run
				 * on this core.
				 */
				rcu_read_unlock();
				schedule();
				rcu_read_lock();
				rpc_count = 0;
			}
		}
		rcu_read_unlock();
		homa_unprotect_rpcs(hsk);
	}
	homa_socktab_end_scan(&scan);
	homa_skb_release_pages(homa);
	homa_peer_gc(homa->peertab);
}
