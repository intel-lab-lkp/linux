// SPDX-License-Identifier: GPL-2.0
#ifndef IOU_WAIT_H
#define IOU_WAIT_H

#include <linux/io_uring_types.h>

/*
 * ->cq_wait_nr is armed with the number of lazy task_work adds the waiter
 * still needs, and counted down by the add side, with the add reaching zero
 * issuing the (single) wake up for this wait cycle. Zero and below means no
 * wake up is to be issued: IO_CQ_WAKE_INIT when no task is waiting (also
 * what a forced wake up resets it to when claiming one), zero once the
 * countdown has fired.
 *
 * An add can stall between publishing its work and claiming the wake up,
 * and the waiter may run that work and arm a later cycle in between. The
 * add then claims the new arm, and if the waiter has not set its task state
 * yet, that wake up is a no-op. Every later add sees the spent arm and
 * issues none. A waiter must therefore test io_cq_wait_arm_spent() after
 * setting its task state and must not sleep if it returns true.
 *
 * The test is sound only if nothing but the waiter stores an arm while it
 * gets ready to sleep, as such a store could restore a spent arm and hide
 * it. io_uring_try_cancel_requests() therefore arms only in the
 * submitter_task's own passes, and the min_wait timer stores an arm only
 * during the timed sleep, which cancels the timer before the waiter tests
 * again.
 */
#define IO_CQ_WAKE_INIT		(-1)

static inline bool io_cq_wait_arm_spent(struct io_ring_ctx *ctx)
{
	return (ctx->flags & IORING_SETUP_DEFER_TASKRUN) &&
	       atomic_read(&ctx->cq_wait_nr) <= 0;
}

struct ext_arg {
	size_t argsz;
	struct timespec64 ts;
	const sigset_t __user *sig;
	ktime_t min_time;
	bool ts_set;
	bool iowait;
};

int io_cqring_wait(struct io_ring_ctx *ctx, int min_events, u32 flags,
		   struct ext_arg *ext_arg);
int io_run_task_work_sig(struct io_ring_ctx *ctx);
void io_cqring_do_overflow_flush(struct io_ring_ctx *ctx);
void io_cqring_overflow_flush_locked(struct io_ring_ctx *ctx);

static inline unsigned int __io_cqring_events(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = io_get_rings(ctx);
	return ctx->cached_cq_tail - READ_ONCE(rings->cq.head);
}

static inline unsigned int __io_cqring_events_user(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = io_get_rings(ctx);

	return READ_ONCE(rings->cq.tail) - READ_ONCE(rings->cq.head);
}

/*
 * Reads the tail/head of the CQ ring while providing an acquire ordering,
 * see comment at top of io_uring.c.
 */
static inline unsigned io_cqring_events(struct io_ring_ctx *ctx)
{
	smp_rmb();
	return __io_cqring_events(ctx);
}

#endif
