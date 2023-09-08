// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2022 Linutronix GmbH, John Ogness
// Copyright (C) 2022 Intel, Thomas Gleixner

#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include "internal.h"
/*
 * Printk console printing implementation for consoles that do not depend on
 * the legacy style console_lock mechanism.
 *
 * Console is locked on a CPU when @nbcon_state::prio is set and
 * @nbcon_state:cpu == current CPU. This is valid for the current execution
 * context.
 *
 * Nesting execution contexts on the same CPU can carefully take over if
 * the driver allows reentrancy via @nbcon_state::unsafe = false. When the
 * interrupted context resumes it checks the state before entering an unsafe
 * region and aborts the operation if it detects a takeover.
 *
 * In case of panic the nesting context can take over the console forcefully.
 * If the interrupted context touches the assigned record buffer after
 * takeover, it does not cause harm because the interrupting single panic
 * context is assigned its own panic record buffer.
 *
 * A concurrent writer on a different CPU with a higher priority can directly
 * take over if the console is not in an unsafe state by carefully writing
 * its priority into @nbcon_state::prio.
 *
 * If the console is in an unsafe state, the concurrent writer with a higher
 * priority can request to take over the console by:
 *
 *	1) Carefully writing the desired priority into @nbcon_state::req_prio
 *	   if there is no higher priority request pending.
 *
 *	2) Carefully spin on @nbcon_state::prio until it is no longer locked.
 *
 *	3) Attempt to lock the console by carefully writing the desired
 *	   priority to @nbcon_state::prio and carefully removing the desired
 *	   priority from @nbcon_state::req_prio.
 *
 * In case the owner does not react on the request and does not make
 * observable progress, the waiter will timeout and can then decide to do
 * an unsafe hostile takeover. Upon unsafe hostile takeover, the console
 * is kept permanently in the unsafe state.
 */

/**
 * nbcon_state_set - Helper function to set the console state
 * @con:	Console to update
 * @new:	The new state to write
 *
 * Only to be used when the console is not yet or no longer visible in the
 * system. Otherwise use nbcon_state_try_cmpxchg().
 */
static inline void nbcon_state_set(struct console *con, struct nbcon_state *new)
{
	atomic_set(&ACCESS_PRIVATE(con, nbcon_state), new->atom);
}

/**
 * nbcon_state_read - Helper function to read the console state
 * @con:	Console to read
 * @state:	The state to store the result
 */
static inline void nbcon_state_read(struct console *con, struct nbcon_state *state)
{
	state->atom = atomic_read(&ACCESS_PRIVATE(con, nbcon_state));
}

/**
 * nbcon_state_try_cmpxchg() - Helper function for atomic_try_cmpxchg() on console state
 * @con:	Console to update
 * @cur:	Old/expected state
 * @new:	New state
 *
 * Return: True on success. False on fail and @cur is updated.
 */
static inline bool nbcon_state_try_cmpxchg(struct console *con, struct nbcon_state *cur,
					   struct nbcon_state *new)
{
	return atomic_try_cmpxchg(&ACCESS_PRIVATE(con, nbcon_state), &cur->atom, new->atom);
}

#ifdef CONFIG_64BIT

#define __seq_to_nbcon_seq(seq) (seq)
#define __nbcon_seq_to_seq(seq) (seq)

#else /* CONFIG_64BIT */

#define __seq_to_nbcon_seq(seq) ((u32)seq)

static inline u64 __nbcon_seq_to_seq(u32 nbcon_seq)
{
	u64 seq;
	u64 rb_next_seq;

	/*
	 * The provided sequence is only the lower 32 bits of the ringbuffer
	 * sequence. It needs to be expanded to 64bit. Get the next sequence
	 * number from the ringbuffer and fold it.
	 *
	 * Having a 32bit representation in the console is sufficient.
	 * If a console ever gets more than 2^31 records behind
	 * the ringbuffer then this is the least of the problems.
	 *
	 * Also the access to the ring buffer is always safe.
	 */
	rb_next_seq = prb_next_seq(prb);
	seq = rb_next_seq - ((u32)rb_next_seq - nbcon_seq);

	return seq;
}

#endif /* CONFIG_64BIT */

/**
 * nbcon_seq_init - Helper function to initialize the console sequence
 * @con:	Console to work on
 *
 * Set @con->nbcon_seq to the starting record (specified with con->seq).
 * If the starting record no longer exists, the oldest available record
 * is chosen. This is especially important on 32bit systems because only
 * the lower 32 bits of the sequence number are stored. The upper 32 bits
 * are derived from the sequence numbers available in the ringbuffer.
 *
 * For init only. Do not use for runtime updates.
 */
static void nbcon_seq_init(struct console *con)
{
	u64 seq = max_t(u64, con->seq, prb_first_valid_seq(prb));

	atomic_long_set(&ACCESS_PRIVATE(con, nbcon_seq), __seq_to_nbcon_seq(seq));

	/* Clear con->seq since nbcon consoles use con->nbcon_seq instead. */
	con->seq = 0;
}

/**
 * nbcon_seq_read - Read the current console sequence
 * @con:	Console to read the sequence of
 *
 * Return:	Sequence number of the next record to print on @con.
 */
u64 nbcon_seq_read(struct console *con)
{
	unsigned long nbcon_seq = atomic_long_read(&ACCESS_PRIVATE(con, nbcon_seq));

	return __nbcon_seq_to_seq(nbcon_seq);
}

/**
 * nbcon_seq_force - Force console sequence to a specific value
 * @con:	Console to work on
 * @seq:	Sequence number value to set
 *
 * Only to be used in extreme situations (such as panic with
 * CONSOLE_REPLAY_ALL).
 */
void nbcon_seq_force(struct console *con, u64 seq)
{
	atomic_long_set(&ACCESS_PRIVATE(con, nbcon_seq), __seq_to_nbcon_seq(seq));
}

/**
 * nbcon_seq_try_update - Try to update the console sequence number
 * @ctxt:	Pointer to an acquire context that contains
 *		all information about the acquire mode
 * @new_seq:	The new sequence number to set
 *
 * @ctxt->seq is updated to the new value of @con::nbcon_seq (expanded to
 * the 64bit value). This could be a different value than @new_seq if
 * nbcon_seq_force() was used or the current context no longer owns the
 * console. In the later case, it will stop printing anyway.
 */
static void nbcon_seq_try_update(struct nbcon_context *ctxt, u64 new_seq)
{
	unsigned long nbcon_seq = __seq_to_nbcon_seq(ctxt->seq);
	struct console *con = ctxt->console;

	if (atomic_long_try_cmpxchg(&ACCESS_PRIVATE(con, nbcon_seq), &nbcon_seq,
				    __seq_to_nbcon_seq(new_seq))) {
		ctxt->seq = new_seq;
	} else {
		ctxt->seq = nbcon_seq_read(con);
	}
}

/**
 * nbcon_context_try_acquire_direct - Try to acquire directly
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * Return:	0 on success and @cur is updated to the new console state.
 *		Otherwise an error code on failure.
 *
 * Errors:
 *
 *	-EPERM:		A panic is in progress and this is not the panic CPU
 *			or this context does not have a priority above the
 *			current owner or waiter. No acquire method can be
 *			successful for this context.
 *
 *	-EBUSY:		The console is in an unsafe state. The caller should
 *			try using the handover acquire method.
 *
 * The general procedure is to change @nbcon_state::prio from unowned to
 * owned. Or, if the console is not in the unsafe state, to change
 * @nbcon_state::prio to a higher priority owner.
 */
static int nbcon_context_try_acquire_direct(struct nbcon_context *ctxt,
					    struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;

	do {
		if (other_cpu_in_panic())
			return -EPERM;

		if (ctxt->prio <= cur->prio || ctxt->prio <= cur->req_prio)
			return -EPERM;

		if (cur->unsafe)
			return -EBUSY;

		/*
		 * The console should never be safe for a direct acquire
		 * if an unsafe hostile takeover has ever happened.
		 */
		WARN_ON_ONCE(cur->unsafe_takeover);

		new.atom = cur->atom;
		new.prio	= ctxt->prio;
		new.req_prio	= NBCON_PRIO_NONE;
		new.unsafe	= cur->unsafe_takeover;
		new.cpu		= cpu;

	} while (!nbcon_state_try_cmpxchg(con, cur, &new));

	cur->atom = new.atom;

	return 0;
}

static bool nbcon_waiter_matches(struct nbcon_state *cur, int expected_prio)
{
	/*
	 * The request context is well defined by the @req_prio because:
	 *
	 * - Only a context with a higher priority can take over the request.
	 * - There are only three priorities.
	 * - Only one CPU is allowed to request PANIC priority.
	 * - Lower priorities are ignored during panic() until reboot.
	 *
	 * As a result, the following scenario is *not* possible:
	 *
	 * 1. Another context with a higher priority directly takes ownership.
	 * 2. The higher priority context releases the ownership.
	 * 3. A lower priority context takes the ownership.
	 * 4. Another context with the same priority as this context
	 *    creates a request and starts waiting.
	 */

	return (cur->req_prio == expected_prio);
}

/**
 * nbcon_context_try_acquire_requested - Try to acquire after having
 *					 requested a handover
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * Return:	0 on success and @cur is updated to the new console state.
 *		Otherwise an error code on failure.
 *
 * Errors:
 *
 *	-EPERM:		A panic is in progress and this is not the panic CPU
 *			or this context is no longer the waiter. For the
 *			former case, the caller must carefully remove the
 *			request before aborting the acquire.
 *
 *	-EBUSY:		The console is still locked. The caller should
 *			continue waiting.
 *
 * This is a helper function for nbcon_context_try_acquire_handover().
 */
static int nbcon_context_try_acquire_requested(struct nbcon_context *ctxt,
					       struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;

	do {
		/*
		 * Note: If the acquire is aborted due to a panic CPU,
		 * the caller must still remove the request!
		 */
		if (other_cpu_in_panic())
			return -EPERM;

		/*
		 * If an unsafe hostile takeover has occurred, a handover
		 * is no longer possible.
		 */
		if (cur->unsafe_takeover)
			return -EPERM;

		/* Is this context still the requester? */
		if (!nbcon_waiter_matches(cur, ctxt->prio))
			return -EPERM;

		/* If still locked, caller should continue waiting. */
		if (cur->prio != NBCON_PRIO_NONE)
			return -EBUSY;

		/*
		 * The previous owner should have never released ownership
		 * in an unsafe region.
		 */
		WARN_ON_ONCE(cur->unsafe);

		new.atom = cur->atom;
		new.prio	= ctxt->prio;
		new.req_prio	= NBCON_PRIO_NONE;
		new.unsafe	= cur->unsafe_takeover;
		new.cpu		= cpu;

	} while (!nbcon_state_try_cmpxchg(con, cur, &new));

	/* Handover success. This context now owns the console. */

	cur->atom = new.atom;

	return 0;
}

/**
 * nbcon_context_try_acquire_handover - Try to acquire via handover
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * Return:	0 on success and @cur is updated to the new console state.
 *		Otherwise an error code on failure.
 *
 * Errors:
 *
 *	-EPERM:		This context is on the same CPU as the current owner
 *			or the console is permanently in an unsafe state or
 *			this context is unwilling to wait or a panic is in
 *			progress and this is not the panic CPU. This is not
 *			the panic context, so no acquire method can be
 *			successful for this context.
 *
 *	-EBUSY:		The current owner or waiter is such that this context
 *			is not able to execute a handover. The caller can use
 *			an unsafe hostile takeover to acquire.
 *
 *	-EAGAIN:	@cur is not the current console state. @cur has been
 *			updated. The caller should retry with direct acquire.
 *
 * The general procedure is to set @req_prio and wait until unowned. Then
 * set @prio (claiming ownership) and clearing @req_prio.
 *
 * Note that it is expected that the caller tried
 * nbcon_context_try_acquire_direct() with @cur before calling this function.
 */
static int nbcon_context_try_acquire_handover(struct nbcon_context *ctxt,
					      struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;
	int timeout;
	int err = 0;

	/*
	 * Handovers are not possible on the same CPU or when the console
	 * is permanently in an unsafe state or if the caller is unwilling
	 * to wait.
	 */
	if (cur->cpu == cpu ||
	    cur->unsafe_takeover ||
	    ctxt->spinwait_max_us == 0) {
		goto fail_handover;
	}

	/* Setup a request for handover. */
	new.atom = cur->atom;
	new.req_prio = ctxt->prio;
	if (!nbcon_state_try_cmpxchg(con, cur, &new))
		return -EAGAIN;

	cur->atom = new.atom;

	/* Wait until there is no owner and then acquire directly. */
	for (timeout = ctxt->spinwait_max_us; timeout >= 0; timeout--) {
		/* On successful acquire, this request is cleared. */
		err = nbcon_context_try_acquire_requested(ctxt, cur);
		if (!err)
			return 0;

		/*
		 * If the acquire should be aborted, it must be ensured
		 * that the request is removed before returning to caller.
		 */
		if (err == -EPERM)
			break;

		udelay(1);

		/* Re-read the state because some time has passed. */
		nbcon_state_read(con, cur);
	}

	/* Timed out or should abort. Carefully remove handover request. */
	do {
		/* No need to remove request if there is a new waiter. */
		if (!nbcon_waiter_matches(cur, ctxt->prio))
			goto fail_handover;

		/* Unset request for handover. */
		new.atom = cur->atom;
		new.req_prio = NBCON_PRIO_NONE;
		if (nbcon_state_try_cmpxchg(con, cur, &new)) {
			/*
			 * Request successfully unset. Report failure of
			 * acquiring via handover.
			 */
			cur->atom = new.atom;
			goto fail_handover;
		}

		/*
		 * Unable to remove request. Try to acquire in case
		 * the owner has released the lock.
		 */
	} while (nbcon_context_try_acquire_requested(ctxt, cur));

	/* Acquire at timeout succeeded! */
	return 0;

fail_handover:
	/*
	 * If this is the panic context, the caller can try to acquire using
	 * the unsafe hostile takeover method.
	 */
	if (ctxt->prio == NBCON_PRIO_PANIC &&
	    ctxt->prio > cur->prio &&
	    ctxt->prio > cur->req_prio &&
	    !other_cpu_in_panic()) {
		return -EBUSY;
	}
	return -EPERM;
}

/**
 * nbcon_context_acquire_hostile - Acquire via unsafe hostile takeover
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * @cur is updated to the new console state.
 *
 * The general procedure is to set @prio (forcing ownership). This method
 * must only be used as a final attempt during panic.
 */
static void nbcon_context_acquire_hostile(struct nbcon_context *ctxt,
					  struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;

	do {
		new.atom = cur->atom;
		new.cpu			= cpu;
		new.prio		= ctxt->prio;
		new.unsafe		|= cur->unsafe_takeover;
		new.unsafe_takeover	|= cur->unsafe;

	} while (!nbcon_state_try_cmpxchg(con, cur, &new));

	cur->atom = new.atom;
}

static struct printk_buffers panic_nbcon_pbufs;

/**
 * nbcon_context_try_acquire - Try to acquire nbcon console
 * @ctxt:	The context of the caller
 *
 * Return:	True if the console was acquired. False otherwise.
 *
 * If the caller allowed an unsafe hostile takeover, on success the
 * caller should check the current console state to see if it is
 * in an unsafe state. Otherwise, on success the caller may assume
 * the console is not in an unsafe state.
 */
__maybe_unused
static bool nbcon_context_try_acquire(struct nbcon_context *ctxt)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state cur;
	int err;

	/* unsafe hostile takeovers are only allowed for panic */
	WARN_ON_ONCE(ctxt->allow_unsafe_takeover && (ctxt->prio != NBCON_PRIO_PANIC));

	nbcon_state_read(con, &cur);

	do {
		err = nbcon_context_try_acquire_direct(ctxt, &cur);
		if (!err)
			goto success;
		else if (err == -EPERM)
			return false;

		err = nbcon_context_try_acquire_handover(ctxt, &cur);
		if (!err)
			goto success;
		else if (err == -EPERM)
			return false;

	} while (err == -EAGAIN);

	/* Only attempt unsafe hostile takeover if explicitly requested. */
	if (!ctxt->allow_unsafe_takeover)
		return false;

	nbcon_context_acquire_hostile(ctxt, &cur);
success:
	/* Assign the appropriate buffer for this context. */
	if (atomic_read(&panic_cpu) == cpu)
		ctxt->pbufs = &panic_nbcon_pbufs;
	else
		ctxt->pbufs = con->pbufs;

	/* Set the record sequence for this context to print. */
	ctxt->seq = nbcon_seq_read(ctxt->console);

	return true;
}

static bool nbcon_owner_matches(struct nbcon_state *cur, int expected_cpu,
				int expected_prio)
{
	/*
	 * Since consoles can only be acquired by higher priorities,
	 * owning contexts are uniquely identified by @prio. However,
	 * since contexts can unexpectedly lose ownership, it is
	 * possible that later another owner appears with the same
	 * priority. For this reason @cpu is also needed.
	 */

	if (cur->prio != expected_prio)
		return false;

	if (cur->cpu != expected_cpu)
		return false;

	return true;
}

/**
 * nbcon_context_release - Release the console
 * @ctxt:	The nbcon context from nbcon_context_try_acquire()
 */
static void nbcon_context_release(struct nbcon_context *ctxt)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state cur;
	struct nbcon_state new;

	nbcon_state_read(con, &cur);

	do {
		if (!nbcon_owner_matches(&cur, cpu, ctxt->prio))
			break;

		new.atom = cur.atom;
		new.prio = NBCON_PRIO_NONE;

		/*
		 * If @unsafe_takeover is set, it is kept set so that
		 * the state remains permanently unsafe.
		 */
		new.unsafe |= cur.unsafe_takeover;

	} while (!nbcon_state_try_cmpxchg(con, &cur, &new));

	ctxt->pbufs = NULL;
}

/**
 * nbcon_context_can_proceed - Check whether ownership can proceed
 * @ctxt:	The nbcon context from nbcon_context_try_acquire()
 * @cur:	The current console state
 *
 * Return:	True if this context still owns the console. False if
 *		ownership was handed over or taken.
 *
 * Must be invoked after the record was dumped into the assigned buffer
 * and at appropriate safe places in the driver.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context.
 */
static bool nbcon_context_can_proceed(struct nbcon_context *ctxt, struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();

	/* Make sure this context still owns the console. */
	if (!nbcon_owner_matches(cur, cpu, ctxt->prio))
		return false;

	/* The console owner can proceed if there is no waiter. */
	if (cur->req_prio == NBCON_PRIO_NONE)
		return true;

	/*
	 * A console owner within an unsafe region is always allowed to
	 * proceed, even if there are waiters. It can perform a handover
	 * when exiting the unsafe region. Otherwise the waiter will
	 * need to perform an unsafe hostile takeover.
	 */
	if (cur->unsafe)
		return true;

	/* Waiters always have higher priorities than owners. */
	WARN_ON_ONCE(cur->req_prio <= cur->prio);

	/*
	 * Having a safe point for take over and eventually a few
	 * duplicated characters or a full line is way better than a
	 * hostile takeover. Post processing can take care of the garbage.
	 * Release and hand over.
	 */
	nbcon_context_release(ctxt);

	/*
	 * It is not clear whether the waiter really took over ownership. The
	 * outermost callsite must make the final decision whether console
	 * ownership is needed for it to proceed. If yes, it must reacquire
	 * ownership (possibly hostile) before carefully proceeding.
	 *
	 * The calling context no longer owns the console so go back all the
	 * way instead of trying to implement reacquire heuristics in tons of
	 * places.
	 */
	return false;
}

#define nbcon_context_enter_unsafe(c)	__nbcon_context_update_unsafe(c, true)
#define nbcon_context_exit_unsafe(c)	__nbcon_context_update_unsafe(c, false)

/**
 * __nbcon_context_update_unsafe - Update the unsafe bit in @con->nbcon_state
 * @ctxt:	The nbcon context from nbcon_context_try_acquire()
 * @unsafe:	The new value for the unsafe bit
 *
 * Return:	True if the unsafe state was updated and this context still
 *		owns the console. Otherwise false if ownership was handed
 *		over or taken.
 *
 * This function allows console owners to modify the unsafe status of the
 * console.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context.
 *
 * Internal helper to avoid duplicated code.
 */
static bool __nbcon_context_update_unsafe(struct nbcon_context *ctxt, bool unsafe)
{
	struct console *con = ctxt->console;
	struct nbcon_state cur;
	struct nbcon_state new;

	nbcon_state_read(con, &cur);

	do {
		/*
		 * The unsafe bit must not be cleared if an
		 * unsafe hostile takeover has occurred.
		 */
		if (!unsafe && cur.unsafe_takeover)
			goto out;

		if (!nbcon_context_can_proceed(ctxt, &cur))
			return false;

		new.atom = cur.atom;
		new.unsafe = unsafe;
	} while (!nbcon_state_try_cmpxchg(con, &cur, &new));

	cur.atom = new.atom;
out:
	return nbcon_context_can_proceed(ctxt, &cur);
}

/**
 * nbcon_emit_next_record - Emit a record in the acquired context
 * @wctxt:	The write context that will be handed to the write function
 *
 * Return:	True if this context still owns the console. False if
 *		ownership was handed over or taken.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context. If the caller
 * wants to do more it must reacquire the console first.
 *
 * When true is returned, @wctxt->ctxt.backlog indicates whether there are
 * still records pending in the ringbuffer,
 */
__maybe_unused
static bool nbcon_emit_next_record(struct nbcon_write_context *wctxt)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(wctxt, ctxt);
	struct console *con = ctxt->console;
	bool is_extended = console_srcu_read_flags(con) & CON_EXTENDED;
	struct printk_message pmsg = {
		.pbufs = ctxt->pbufs,
	};
	unsigned long con_dropped;
	struct nbcon_state cur;
	unsigned long dropped;
	bool done;

	/*
	 * The printk buffers are filled within an unsafe section. This
	 * prevents NBCON_PRIO_NORMAL and NBCON_PRIO_EMERGENCY from
	 * clobbering each other.
	 */

	if (!nbcon_context_enter_unsafe(ctxt))
		return false;

	ctxt->backlog = printk_get_next_message(&pmsg, ctxt->seq, is_extended, true);
	if (!ctxt->backlog)
		return nbcon_context_exit_unsafe(ctxt);

	/*
	 * @con->dropped is not protected in case of an unsafe hostile
	 * takeover. In that situation the update can be racy so
	 * annotate it accordingly.
	 */
	con_dropped = data_race(READ_ONCE(con->dropped));

	dropped = con_dropped + pmsg.dropped;
	if (dropped && !is_extended)
		console_prepend_dropped(&pmsg, dropped);

	if (!nbcon_context_exit_unsafe(ctxt))
		return false;

	/* For skipped records just update seq/dropped in @con. */
	if (pmsg.outbuf_len == 0)
		goto update_con;

	/* Initialize the write context for driver callbacks. */
	wctxt->outbuf = &pmsg.pbufs->outbuf[0];
	wctxt->len = pmsg.outbuf_len;
	nbcon_state_read(con, &cur);
	wctxt->unsafe_takeover = cur.unsafe_takeover;

	if (con->write_atomic) {
		done = con->write_atomic(con, wctxt);
	} else {
		nbcon_context_release(ctxt);
		WARN_ON_ONCE(1);
		done = false;
	}

	/* If not done, the emit was aborted. */
	if (!done)
		return false;

	/*
	 * Since any dropped message was successfully output, reset the
	 * dropped count for the console.
	 */
	dropped = 0;
update_con:
	/*
	 * The dropped count and the sequence number are updated within an
	 * unsafe section. This limits update races to the panic context and
	 * allows the panic context to win.
	 */

	if (!nbcon_context_enter_unsafe(ctxt))
		return false;

	if (dropped != con_dropped) {
		/* Counterpart to the READ_ONCE() above. */
		WRITE_ONCE(con->dropped, dropped);
	}

	nbcon_seq_try_update(ctxt, pmsg.seq + 1);

	return nbcon_context_exit_unsafe(ctxt);
}

/**
 * nbcon_alloc - Allocate buffers needed by the nbcon console
 * @con:	Console to allocate buffers for
 *
 * Return:	True on success. False otherwise and the console cannot
 *		be used.
 *
 * This is not part of nbcon_init() because buffer allocation must
 * be performed earlier in the console registration process.
 */
bool nbcon_alloc(struct console *con)
{
	if (con->flags & CON_BOOT) {
		/*
		 * Boot console printing is synchronized with legacy console
		 * printing, so boot consoles can share the same global printk
		 * buffers.
		 */
		con->pbufs = &printk_shared_pbufs;
	} else {
		con->pbufs = kmalloc(sizeof(*con->pbufs), GFP_KERNEL);
		if (!con->pbufs) {
			con_printk(KERN_ERR, con, "failed to allocate printing buffer\n");
			return false;
		}
	}

	return true;
}

/**
 * nbcon_init - Initialize the nbcon console specific data
 * @con:	Console to initialize
 *
 * nbcon_alloc() *must* be called and succeed before this function
 * is called.
 *
 * This function expects that the legacy @con->seq has been set.
 */
void nbcon_init(struct console *con)
{
	struct nbcon_state state = { };

	/* nbcon_alloc() must have been called and successful! */
	BUG_ON(!con->pbufs);

	nbcon_seq_init(con);
	nbcon_state_set(con, &state);
}

/**
 * nbcon_free - Free and cleanup the nbcon console specific data
 * @con:	Console to free/cleanup nbcon data
 */
void nbcon_free(struct console *con)
{
	struct nbcon_state state = { };

	nbcon_state_set(con, &state);

	/* Boot consoles share global printk buffers. */
	if (!(con->flags & CON_BOOT))
		kfree(con->pbufs);

	con->pbufs = NULL;
}
