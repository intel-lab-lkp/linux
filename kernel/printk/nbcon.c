// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2022 Linutronix GmbH, John Ogness
// Copyright (C) 2022 Intel, Thomas Gleixner

#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/delay.h>
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
		 * Direct acquires should never be attempted if
		 * an unsafe hostile takeover has ever happened.
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

static bool nbcon_waiter_matches(struct nbcon_state *cur, int expected_tag,
				int expected_prio)
{
	/*
	 * Since consoles can only be acquired by higher priorities,
	 * waiting contexts are uniquely identified by @prio. However,
	 * since owners and waiters can unexpectedly change, it is
	 * possible that later another waiter appears with the same
	 * priority. For this reason @req_tag is also needed.
	 *
	 * Using the waiting CPU would be better, but there are not enough
	 * bits in the state variable for this. Since unexpected waiter
	 * changes are rare and them going unnoticed does not cause fatal
	 * problems, the tagged bits should be sufficient.
	 */

	if (cur->req_prio != expected_prio)
		return false;

	if (cur->req_tag != expected_tag)
		return false;

	return true;
}

/**
 * nbcon_context_try_acquire_requested - Try to acquire after having
 *					 requested a handover
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 * @req_tag:	The tagged bits used to identify this waiting context
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
 *
 * The use of tagged bits is to partially deal with the situation that while
 * this waiting context is in udelay(1):
 *
 *   1. another context with higher priority directly takes ownership
 *   2. the higher priority context releases ownership
 *   3. a lower priority context takes ownership
 *   4. a context with the same priority as this context requests ownership
 *   5. this waiting context finishes udelay(1) and thinks it is the waiter
 *
 * To address this rare situation, tagged bits are used so that the waiter
 * can better identify if it is really the waiter. In the above  example, the
 * original waiter would use a @req_tag value of 1, whereas the follow-up
 * waiter would use a @req_tag value of 2. This allows the original waiter to
 * identify in step 5 that it has been replaced by another waiter.
 */
static int nbcon_context_try_acquire_requested(struct nbcon_context *ctxt,
					       struct nbcon_state *cur,
					       char req_tag)
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
		if (!nbcon_waiter_matches(cur, req_tag, ctxt->prio))
			return -EPERM;

		/* If still locked, caller should continue waiting. */
		if (cur->prio != NBCON_PRIO_NONE)
			return -EBUSY;

		/* Handover acquires should never be attempted if unsafe. */
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
	int req_tag;
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

	/*
	 * Setup a request for handover.
	 *
	 * @nbcon_state::req_tag is modified for each handover request. See
	 * the function description of nbcon_context_try_acquire_requested()
	 * for details about how @req_tag is used.
	 */
	new.atom = cur->atom;
	new.req_prio = ctxt->prio;
	new.req_tag++;
	req_tag = new.req_tag;
	if (!nbcon_state_try_cmpxchg(con, cur, &new))
		return -EAGAIN;

	cur->atom = new.atom;

	/* Wait until there is no owner and then acquire directly. */
	for (timeout = ctxt->spinwait_max_us; timeout >= 0; timeout--) {
		/* On successful acquire, this request is cleared. */
		err = nbcon_context_try_acquire_requested(ctxt, cur, req_tag);
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
		if (!nbcon_waiter_matches(cur, req_tag, ctxt->prio))
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
	} while (nbcon_context_try_acquire_requested(ctxt, cur, req_tag));

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
 * nbcon_context_try_acquire_hostile - Acquire via unsafe hostile takeover
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
	__maybe_unused
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
__maybe_unused
static void nbcon_context_release(struct nbcon_context *ctxt)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state cur;
	struct nbcon_state new;

	nbcon_state_read(con, &cur);

	do {
		if (!nbcon_owner_matches(&cur, cpu, ctxt->prio))
			return;

		new.atom = cur.atom;
		new.prio = NBCON_PRIO_NONE;

		/*
		 * If @unsafe_takeover is set, it is kept set so that
		 * the state remains permanently unsafe.
		 */
		new.unsafe |= cur.unsafe_takeover;

	} while (!nbcon_state_try_cmpxchg(con, &cur, &new));
}

/**
 * nbcon_init - Initialize the nbcon console specific data
 * @con:	Console to initialize
 */
void nbcon_init(struct console *con)
{
	struct nbcon_state state = { };

	nbcon_state_set(con, &state);
}

/**
 * nbcon_cleanup - Cleanup the nbcon console specific data
 * @con:	Console to cleanup
 */
void nbcon_cleanup(struct console *con)
{
	struct nbcon_state state = { };

	nbcon_state_set(con, &state);
}
