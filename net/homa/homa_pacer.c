// SPDX-License-Identifier: BSD-2-Clause

/* This file implements the Homa pacer, which implements SRPT for packet
 * output. In order to do that, it throttles packet transmission to prevent
 * the buildup of large queues in the NIC.
 */

#include "homa_pacer.h"
#include "homa_rpc.h"

/**
 * homa_pacer_new() - Allocate and initialize a new pacer object, which
 * will hold pacer-related information for @homa.
 * @homa:   Homa transport that the pacer will be associated with.
 * @net:    Network namespace that @homa is associated with.
 * Return:  A pointer to the new struct pacer, or a negative errno.
 */
struct homa_pacer *homa_pacer_new(struct homa *homa, struct net *net)
{
	struct homa_pacer *pacer;
	int err;

	pacer = kmalloc(sizeof(*pacer), GFP_KERNEL | __GFP_ZERO);
	if (!pacer)
		return ERR_PTR(-ENOMEM);
	pacer->homa = homa;
	spin_lock_init(&pacer->mutex);
	pacer->fifo_count = 1000;
	spin_lock_init(&pacer->throttle_lock);
	INIT_LIST_HEAD_RCU(&pacer->throttled_rpcs);
	pacer->fifo_fraction = 50;
	pacer->max_nic_queue_ns = 5000;
	pacer->link_mbps = 25000;
	pacer->throttle_min_bytes = 1000;
	pacer->exit = false;
	init_waitqueue_head(&pacer->wait_queue);
	pacer->kthread = kthread_run(homa_pacer_main, pacer, "homa_pacer");
	if (IS_ERR(pacer->kthread)) {
		err = PTR_ERR(pacer->kthread);
		pr_err("Homa couldn't create pacer thread: error %d\n", err);
		goto error;
	}
	init_completion(&pacer->kthread_done);
	atomic64_set(&pacer->link_idle_time, sched_clock());

	homa_pacer_update_sysctl_deps(pacer);
	return pacer;

error:
	homa_pacer_destroy(pacer);
	return ERR_PTR(err);
}

/**
 * homa_pacer_destroy() - Cleanup and destroy the pacer object for a Homa
 * transport.
 * @pacer:    Object to destroy; caller must not reference the object
 *            again once this function returns.
 */
void homa_pacer_destroy(struct homa_pacer *pacer)
{
	pacer->exit = true;
	if (pacer->kthread) {
		wake_up(&pacer->wait_queue);
		kthread_stop(pacer->kthread);
		wait_for_completion(&pacer->kthread_done);
		pacer->kthread = NULL;
	}
	kfree(pacer);
}

/**
 * homa_pacer_check_nic_q() - This function is invoked before passing a
 * packet to the NIC for transmission. It serves two purposes. First, it
 * maintains an estimate of the NIC queue length. Second, it indicates to
 * the caller whether the NIC queue is so full that no new packets should be
 * queued (Homa's SRPT depends on keeping the NIC queue short).
 * @pacer:    Pacer information for a Homa transport.
 * @skb:      Packet that is about to be transmitted.
 * @force:    True means this packet is going to be transmitted
 *            regardless of the queue length.
 * Return:    Nonzero is returned if either the NIC queue length is
 *            acceptably short or @force was specified. 0 means that the
 *            NIC queue is at capacity or beyond, so the caller should delay
 *            the transmission of @skb. If nonzero is returned, then the
 *            queue estimate is updated to reflect the transmission of @skb.
 */
int homa_pacer_check_nic_q(struct homa_pacer *pacer, struct sk_buff *skb,
			   bool force)
{
	u64 idle, new_idle, clock, ns_for_packet;
	int bytes;

	bytes = homa_get_skb_info(skb)->wire_bytes;
	ns_for_packet = pacer->ns_per_mbyte;
	ns_for_packet *= bytes;
	do_div(ns_for_packet, 1000000);
	while (1) {
		clock = sched_clock();
		idle = atomic64_read(&pacer->link_idle_time);
		if ((clock + pacer->max_nic_queue_ns) < idle && !force &&
		    !(pacer->homa->flags & HOMA_FLAG_DONT_THROTTLE))
			return 0;
		if (idle < clock)
			new_idle = clock + ns_for_packet;
		else
			new_idle = idle + ns_for_packet;

		/* This method must be thread-safe. */
		if (atomic64_cmpxchg_relaxed(&pacer->link_idle_time, idle,
					     new_idle) == idle)
			break;
	}
	return 1;
}

/**
 * homa_pacer_main() - Top-level function for the pacer thread.
 * @arg:  Pointer to pacer struct.
 *
 * Return:         Always 0.
 */
int homa_pacer_main(void *arg)
{
	struct homa_pacer *pacer = arg;

	while (1) {
		if (pacer->exit)
			break;
		pacer->wake_time = sched_clock();
		homa_pacer_xmit(pacer);
		pacer->wake_time = 0;
		if (!list_empty(&pacer->throttled_rpcs)) {
			/* NIC queue is full; before calling pacer again,
			 * give other threads a chance to run (otherwise
			 * low-level packet processing such as softirq could
			 * get locked out).
			 */
			schedule();
			continue;
		}

		wait_event(pacer->wait_queue, pacer->exit ||
			   !list_empty(&pacer->throttled_rpcs));
	}
	kthread_complete_and_exit(&pacer->kthread_done, 0);
	return 0;
}

/**
 * homa_pacer_xmit() - Transmit packets from  the throttled list until
 * either (a) the throttled list is empty or (b) the NIC queue has
 * reached maximum allowable length. Note: this function may be invoked
 * from either process context or softirq (BH) level. This function is
 * invoked from multiple places, not just in the pacer thread. The reason
 * for this is that (as of 10/2019) Linux's scheduling of the pacer thread
 * is unpredictable: the thread may block for long periods of time (e.g.,
 * because it is assigned to the same CPU as a busy interrupt handler).
 * This can result in poor utilization of the network link. So, this method
 * gets invoked from other places as well, to increase the likelihood that we
 * keep the link busy. Those other invocations are not guaranteed to happen,
 * so the pacer thread provides a backstop.
 * @pacer:    Pacer information for a Homa transport.
 */
void homa_pacer_xmit(struct homa_pacer *pacer)
{
	struct homa_rpc *rpc;
	s64 queue_ns;

	/* Make sure only one instance of this function executes at a time. */
	if (!spin_trylock_bh(&pacer->mutex))
		return;

	while (1) {
		queue_ns = atomic64_read(&pacer->link_idle_time) - sched_clock();
		if (queue_ns >= pacer->max_nic_queue_ns)
			break;
		if (list_empty(&pacer->throttled_rpcs))
			break;

		/* Lock the first throttled RPC. This may not be possible
		 * because we have to hold throttle_lock while locking
		 * the RPC; that means we can't wait for the RPC lock because
		 * of lock ordering constraints (see sync.txt). Thus, if
		 * the RPC lock isn't available, do nothing. Holding the
		 * throttle lock while locking the RPC is important because
		 * it keeps the RPC from being deleted before it can be locked.
		 */
		homa_pacer_throttle_lock(pacer);
		pacer->fifo_count -= pacer->fifo_fraction;
		if (pacer->fifo_count <= 0) {
			struct homa_rpc *cur;
			u64 oldest = ~0;

			pacer->fifo_count += 1000;
			rpc = NULL;
			list_for_each_entry(cur, &pacer->throttled_rpcs,
						throttled_links) {
				if (cur->msgout.init_ns < oldest) {
					rpc = cur;
					oldest = cur->msgout.init_ns;
				}
			}
		} else {
			rpc = list_first_entry_or_null(&pacer->throttled_rpcs,
						       struct homa_rpc,
						       throttled_links);
		}
		if (!rpc) {
			homa_pacer_throttle_unlock(pacer);
			break;
		}
		if (!homa_rpc_try_lock(rpc)) {
			homa_pacer_throttle_unlock(pacer);
			break;
		}
		homa_pacer_throttle_unlock(pacer);

		homa_xmit_data(rpc, true);

		/* Note: rpc->state could be RPC_DEAD here, but the code
		 * below should work anyway.
		 */
		if (!*rpc->msgout.next_xmit)
			/* No more data can be transmitted from this message
			 * (right now), so remove it from the throttled list.
			 */
			homa_pacer_unmanage_rpc(rpc);
		homa_rpc_unlock(rpc);
	}
	spin_unlock_bh(&pacer->mutex);
}

/**
 * homa_pacer_manage_rpc() - Arrange for the pacer to transmit packets
 * from this RPC (make sure that an RPC is on the throttled list and wake up
 * the pacer thread if necessary).
 * @rpc:     RPC with outbound packets that have been granted but can't be
 *           sent because of NIC queue restrictions. Must be locked by caller.
 */
void homa_pacer_manage_rpc(struct homa_rpc *rpc)
	__must_hold(rpc_bucket_lock)
{
	struct homa_pacer *pacer = rpc->hsk->homa->pacer;
	struct homa_rpc *candidate;
	int bytes_left;
	int checks = 0;

	if (!list_empty(&rpc->throttled_links))
		return;
	bytes_left = rpc->msgout.length - rpc->msgout.next_xmit_offset;
	homa_pacer_throttle_lock(pacer);
	list_for_each_entry(candidate, &pacer->throttled_rpcs,
				throttled_links) {
		int bytes_left_cand;

		checks++;

		/* Watch out: the pacer might have just transmitted the last
		 * packet from candidate.
		 */
		bytes_left_cand = candidate->msgout.length -
				candidate->msgout.next_xmit_offset;
		if (bytes_left_cand > bytes_left) {
			list_add_tail(&rpc->throttled_links,
					  &candidate->throttled_links);
			goto done;
		}
	}
	list_add_tail(&rpc->throttled_links, &pacer->throttled_rpcs);
done:
	homa_pacer_throttle_unlock(pacer);
	wake_up(&pacer->wait_queue);
}

/**
 * homa_pacer_unmanage_rpc() - Make sure that an RPC is no longer managed
 * by the pacer.
 * @rpc:     RPC of interest.
 */
void homa_pacer_unmanage_rpc(struct homa_rpc *rpc)
	__must_hold(rpc_bucket_lock)
{
	struct homa_pacer *pacer = rpc->hsk->homa->pacer;

	if (unlikely(!list_empty(&rpc->throttled_links))) {
		homa_pacer_throttle_lock(pacer);
		list_del_init(&rpc->throttled_links);
		homa_pacer_throttle_unlock(pacer);
	}
}

/**
 * homa_pacer_update_sysctl_deps() - Update any pacer fields that depend
 * on values set by sysctl. This function is invoked anytime a pacer sysctl
 * value is updated.
 * @pacer:   Pacer to update.
 */
void homa_pacer_update_sysctl_deps(struct homa_pacer *pacer)
{
	u64 tmp;

	tmp = 8 * 1000ULL * 1000ULL * 1000ULL;

	/* Underestimate link bandwidth (overestimate time) by 1%. */
	tmp = tmp * 101 / 100;
	do_div(tmp, pacer->link_mbps);
	pacer->ns_per_mbyte = tmp;
}

