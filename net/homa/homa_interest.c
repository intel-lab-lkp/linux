// SPDX-License-Identifier: BSD-2-Clause or GPL-2.0+

/* This file contains functions for managing homa_interest structs. */

#include "homa_impl.h"
#include "homa_interest.h"
#include "homa_rpc.h"
#include "homa_sock.h"

/**
 * homa_interest_init_shared() - Initialize an interest and queue it up on
 * a socket.
 * @interest:  Interest to initialize
 * @hsk:       Socket on which the interests should be queued. Must be locked
 *             by caller.
 */
void homa_interest_init_shared(struct homa_interest *interest,
			       struct homa_sock *hsk)
	__must_hold(hsk->lock)
{
	interest->rpc = NULL;
	atomic_set(&interest->ready, 0);
	interest->blocked = 0;
	init_waitqueue_head(&interest->wait_queue);
	interest->hsk = hsk;
	list_add(&interest->links, &hsk->interests);
}

/**
 * homa_interest_init_private() - Initialize an interest that will wait
 * on a particular (private) RPC, and link it to that RPC.
 * @interest:   Interest to initialize.
 * @rpc:        RPC to associate with the interest. Must be private, and
 *              caller must have locked it.
 *
 * Return:      0 for success, otherwise a negative errno.
 */
int homa_interest_init_private(struct homa_interest *interest,
			       struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	if (rpc->private_interest)
		return -EINVAL;

	interest->rpc = rpc;
	atomic_set(&interest->ready, 0);
	interest->blocked = 0;
	init_waitqueue_head(&interest->wait_queue);
	interest->hsk = rpc->hsk;
	rpc->private_interest = interest;
	return 0;
}

/**
 * homa_interest_wait() - Wait for an interest to have an actionable RPC,
 * or for an error to occur.
 * @interest:     Interest to wait for; must previously have been initialized
 *                and linked to a socket or RPC. On return, the interest
 *                will have been unlinked if its ready flag is set; otherwise
 *                it may still be linked.
 *
 * Return: 0 for success (the ready flag is set in the interest), or -EINTR
 * if the thread received an interrupt.
 */
int homa_interest_wait(struct homa_interest *interest)
{
	struct homa_sock *hsk = interest->hsk;
	int result = 0;
	int iteration;
	int wait_err;

	interest->blocked = 0;

	/* This loop iterates in order to poll and/or reap dead RPCS. */
	for (iteration = 0; ; iteration++) {
		if (iteration != 0)
			/* Give NAPI/SoftIRQ tasks a chance to run. */
			schedule();

		if (atomic_read_acquire(&interest->ready) != 0)
			goto done;

		/* See if we can cleanup dead RPCs while waiting. */
		if (homa_rpc_reap(hsk, false) != 0)
			continue;

		break;
	}

	interest->blocked = 1;
	wait_err = wait_event_interruptible_exclusive(interest->wait_queue,
						      atomic_read_acquire(&interest->ready) != 0);
	if (wait_err == -ERESTARTSYS)
		result = -EINTR;

done:
	return result;
}

/**
 * homa_interest_notify_private() - If a thread is waiting on the private
 * interest for an RPC, wake it up.
 * @rpc:      RPC that may (potentially) have a private interest. Must be
 *            locked by the caller.
 */
void homa_interest_notify_private(struct homa_rpc *rpc)
	__must_hold(rpc->bucket->lock)
{
	if (rpc->private_interest) {
		atomic_set_release(&rpc->private_interest->ready, 1);
		wake_up(&rpc->private_interest->wait_queue);
	}
}

