// SPDX-License-Identifier: GPL-2.0-only
/*
 * Folio bit-lock and wait-queue infrastructure.
 */
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/leafops.h>
#include <linux/pagemap.h>
#include <linux/wait.h>
#include <linux/hash.h>
#include <linux/sysctl.h>
#include <linux/delayacct.h>
#include <linux/psi.h>
#include <linux/migrate.h>

#include "internal.h"

/*
 * In order to wait for pages to become available there must be
 * waitqueues associated with pages. By using a hash table of
 * waitqueues where the bucket discipline is to maintain all
 * waiters on the same queue and wake all when any of the pages
 * become available, and for the woken contexts to check to be
 * sure the appropriate page became available, this saves space
 * at a cost of "thundering herd" phenomena during rare hash
 * collisions.
 */
#define PAGE_WAIT_TABLE_BITS 8
#define PAGE_WAIT_TABLE_SIZE (1 << PAGE_WAIT_TABLE_BITS)
static wait_queue_head_t folio_wait_table[PAGE_WAIT_TABLE_SIZE] __cacheline_aligned;

static wait_queue_head_t *folio_waitqueue(struct folio *folio)
{
	return &folio_wait_table[hash_ptr(folio, PAGE_WAIT_TABLE_BITS)];
}

/* How many times do we accept lock stealing from under a waiter? */
static int sysctl_page_lock_unfairness = 5;
static const struct ctl_table folio_wait_sysctl_table[] = {
	{
		.procname	= "page_lock_unfairness",
		.data		= &sysctl_page_lock_unfairness,
		.maxlen		= sizeof(sysctl_page_lock_unfairness),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	}
};

void __init folio_wait_init(void)
{
	int i;

	for (i = 0; i < PAGE_WAIT_TABLE_SIZE; i++)
		init_waitqueue_head(&folio_wait_table[i]);

	register_sysctl_init("vm", folio_wait_sysctl_table);
}

/*
 * The page wait code treats the "wait->flags" somewhat unusually, because
 * we have multiple different kinds of waits, not just the usual "exclusive"
 * one.
 *
 * We have:
 *
 *  (a) no special bits set:
 *
 *	We're just waiting for the bit to be released, and when a waker
 *	calls the wakeup function, we set WQ_FLAG_WOKEN and wake it up,
 *	and remove it from the wait queue.
 *
 *	Simple and straightforward.
 *
 *  (b) WQ_FLAG_EXCLUSIVE:
 *
 *	The waiter is waiting to get the lock, and only one waiter should
 *	be woken up to avoid any thundering herd behavior. We'll set the
 *	WQ_FLAG_WOKEN bit, wake it up, and remove it from the wait queue.
 *
 *	This is the traditional exclusive wait.
 *
 *  (c) WQ_FLAG_EXCLUSIVE | WQ_FLAG_CUSTOM:
 *
 *	The waiter is waiting to get the bit, and additionally wants the
 *	lock to be transferred to it for fair lock behavior. If the lock
 *	cannot be taken, we stop walking the wait queue without waking
 *	the waiter.
 *
 *	This is the "fair lock handoff" case, and in addition to setting
 *	WQ_FLAG_WOKEN, we set WQ_FLAG_DONE to let the waiter easily see
 *	that it now has the lock.
 */
static int wake_page_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *arg)
{
	unsigned int flags;
	struct wait_page_key *key = arg;
	struct wait_page_queue *wait_page
		= container_of(wait, struct wait_page_queue, wait);

	if (!wake_page_match(wait_page, key))
		return 0;

	/*
	 * If it's a lock handoff wait, we get the bit for it, and
	 * stop walking (and do not wake it up) if we can't.
	 */
	flags = wait->flags;
	if (flags & WQ_FLAG_EXCLUSIVE) {
		if (test_bit(key->bit_nr, &key->folio->flags.f))
			return -1;
		if (flags & WQ_FLAG_CUSTOM) {
			if (test_and_set_bit(key->bit_nr, &key->folio->flags.f))
				return -1;
			flags |= WQ_FLAG_DONE;
		}
	}

	/*
	 * We are holding the wait-queue lock, but the waiter that
	 * is waiting for this will be checking the flags without
	 * any locking.
	 *
	 * So update the flags atomically, and wake up the waiter
	 * afterwards to avoid any races. This store-release pairs
	 * with the load-acquire in folio_wait_bit_common().
	 */
	smp_store_release(&wait->flags, flags | WQ_FLAG_WOKEN);
	wake_up_state(wait->private, mode);

	/*
	 * Ok, we have successfully done what we're waiting for,
	 * and we can unconditionally remove the wait entry.
	 *
	 * Note that this pairs with the "finish_wait()" in the
	 * waiter, and has to be the absolute last thing we do.
	 * After this list_del_init(&wait->entry) the wait entry
	 * might be de-allocated and the process might even have
	 * exited.
	 */
	list_del_init_careful(&wait->entry);
	return (flags & WQ_FLAG_EXCLUSIVE) != 0;
}

static void folio_wake_bit(struct folio *folio, int bit_nr)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	struct wait_page_key key;
	unsigned long flags;

	key.folio = folio;
	key.bit_nr = bit_nr;
	key.page_match = 0;

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_locked_key(q, TASK_NORMAL, &key);

	/*
	 * It's possible to miss clearing waiters here, when we woke our page
	 * waiters, but the hashed waitqueue has waiters for other pages on it.
	 * That's okay, it's a rare case. The next waker will clear it.
	 *
	 * Note that, depending on the page pool (buddy, hugetlb, ZONE_DEVICE,
	 * other), the flag may be cleared in the course of freeing the page;
	 * but that is not required for correctness.
	 */
	if (!waitqueue_active(q) || !key.page_match)
		folio_clear_waiters(folio);

	spin_unlock_irqrestore(&q->lock, flags);
}

/*
 * Wake waiters on PG_writeback for @folio.
 */
void folio_wake_writeback(struct folio *folio)
{
	folio_wake_bit(folio, PG_writeback);
}

/*
 * A choice of three behaviors for folio_wait_bit_common():
 */
enum behavior {
	EXCLUSIVE,	/* Hold ref to page and take the bit when woken, like
			 * __folio_lock() waiting on then setting PG_locked.
			 */
	SHARED,		/* Hold ref to page and check the bit when woken, like
			 * folio_wait_writeback() waiting on PG_writeback.
			 */
	DROP,		/* Drop ref to page before wait, no check when woken,
			 * like folio_put_wait_locked() on PG_locked.
			 */
};

/*
 * Attempt to check (or get) the folio flag, and mark us done
 * if successful.
 */
static inline bool folio_trylock_flag(struct folio *folio, int bit_nr,
					struct wait_queue_entry *wait)
{
	if (wait->flags & WQ_FLAG_EXCLUSIVE) {
		if (test_and_set_bit(bit_nr, &folio->flags.f))
			return false;
	} else if (test_bit(bit_nr, &folio->flags.f))
		return false;

	wait->flags |= WQ_FLAG_WOKEN | WQ_FLAG_DONE;
	return true;
}

static inline int folio_wait_bit_common(struct folio *folio, int bit_nr,
		int state, enum behavior behavior)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	int unfairness = sysctl_page_lock_unfairness;
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	unsigned long pflags;
	bool in_thrashing;

	if (bit_nr == PG_locked &&
	    !folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		delayacct_thrashing_start(&in_thrashing);
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = bit_nr;

repeat:
	wait->flags = 0;
	if (behavior == EXCLUSIVE) {
		wait->flags = WQ_FLAG_EXCLUSIVE;
		if (--unfairness < 0)
			wait->flags |= WQ_FLAG_CUSTOM;
	}

	/*
	 * Do one last check whether we can get the
	 * page bit synchronously.
	 *
	 * Do the folio_set_waiters() marking before that
	 * to let any waker we _just_ missed know they
	 * need to wake us up (otherwise they'll never
	 * even go to the slow case that looks at the
	 * page queue), and add ourselves to the wait
	 * queue if we need to sleep.
	 *
	 * This part needs to be done under the queue
	 * lock to avoid races.
	 */
	spin_lock_irq(&q->lock);
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, bit_nr, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * From now on, all the logic will be based on
	 * the WQ_FLAG_WOKEN and WQ_FLAG_DONE flag, to
	 * see whether the page bit testing has already
	 * been done by the wake function.
	 *
	 * We can drop our reference to the folio.
	 */
	if (behavior == DROP)
		folio_put(folio);

	/*
	 * Note that until the "finish_wait()", or until
	 * we see the WQ_FLAG_WOKEN flag, we need to
	 * be very careful with the 'wait->flags', because
	 * we may race with a waker that sets them.
	 */
	for (;;) {
		unsigned int flags;

		set_current_state(state);

		/* Loop until we've been woken or interrupted */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(state, current))
				break;

			io_schedule();
			continue;
		}

		/* If we were non-exclusive, we're done */
		if (behavior != EXCLUSIVE)
			break;

		/* If the waker got the lock for us, we're done */
		if (flags & WQ_FLAG_DONE)
			break;

		/*
		 * Otherwise, if we're getting the lock, we need to
		 * try to get it ourselves.
		 *
		 * And if that fails, we'll have to retry this all.
		 */
		if (unlikely(test_and_set_bit(bit_nr, folio_flags(folio, 0))))
			goto repeat;

		wait->flags |= WQ_FLAG_DONE;
		break;
	}

	/*
	 * If a signal happened, this 'finish_wait()' may remove the last
	 * waiter from the wait-queues, but the folio waiters bit will remain
	 * set. That's ok. The next wakeup will take care of it, and trying
	 * to do it here would be difficult and prone to races.
	 */
	finish_wait(q, wait);

	if (thrashing) {
		delayacct_thrashing_end(&in_thrashing);
		psi_memstall_leave(&pflags);
	}

	/*
	 * NOTE! The wait->flags weren't stable until we've done the
	 * 'finish_wait()', and we could have exited the loop above due
	 * to a signal, and had a wakeup event happen after the signal
	 * test but before the 'finish_wait()'.
	 *
	 * So only after the finish_wait() can we reliably determine
	 * if we got woken up or not, so we can now figure out the final
	 * return value based on that state without races.
	 *
	 * Also note that WQ_FLAG_WOKEN is sufficient for a non-exclusive
	 * waiter, but an exclusive one requires WQ_FLAG_DONE.
	 */
	if (behavior == EXCLUSIVE)
		return wait->flags & WQ_FLAG_DONE ? 0 : -EINTR;

	return wait->flags & WQ_FLAG_WOKEN ? 0 : -EINTR;
}

#ifdef CONFIG_MIGRATION
/**
 * softleaf_entry_wait_on_locked - Wait for a migration entry or
 * device_private entry to be removed.
 * @entry: migration or device_private swap entry.
 * @ptl: already locked ptl. This function will drop the lock.
 *
 * Wait for a migration entry referencing the given page, or device_private
 * entry referencing a dvice_private page to be unlocked. This is
 * equivalent to folio_put_wait_locked(folio, TASK_UNINTERRUPTIBLE) except
 * this can be called without taking a reference on the page. Instead this
 * should be called while holding the ptl for @entry referencing
 * the page.
 *
 * Returns after unlocking the ptl.
 *
 * This follows the same logic as folio_wait_bit_common() so see the comments
 * there.
 */
void softleaf_entry_wait_on_locked(softleaf_t entry, spinlock_t *ptl)
	__releases(ptl)
{
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	unsigned long pflags;
	bool in_thrashing;
	wait_queue_head_t *q;
	struct folio *folio = softleaf_to_folio(entry);

	q = folio_waitqueue(folio);
	if (!folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		delayacct_thrashing_start(&in_thrashing);
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = PG_locked;
	wait->flags = 0;

	spin_lock_irq(&q->lock);
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, PG_locked, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * If a migration entry exists for the page the migration path must hold
	 * a valid reference to the page, and it must take the ptl to remove the
	 * migration entry. So the page is valid until the ptl is dropped.
	 * Similarly any path attempting to drop the last reference to a
	 * device-private page needs to grab the ptl to remove the device-private
	 * entry.
	 */
	spin_unlock(ptl);

	for (;;) {
		unsigned int flags;

		set_current_state(TASK_UNINTERRUPTIBLE);

		/* Loop until we've been woken or interrupted */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(TASK_UNINTERRUPTIBLE, current))
				break;

			io_schedule();
			continue;
		}
		break;
	}

	finish_wait(q, wait);

	if (thrashing) {
		delayacct_thrashing_end(&in_thrashing);
		psi_memstall_leave(&pflags);
	}
}
#endif

void folio_wait_bit(struct folio *folio, int bit_nr)
{
	folio_wait_bit_common(folio, bit_nr, TASK_UNINTERRUPTIBLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit);

int folio_wait_bit_killable(struct folio *folio, int bit_nr)
{
	return folio_wait_bit_common(folio, bit_nr, TASK_KILLABLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit_killable);

/**
 * folio_put_wait_locked - Drop a reference and wait for it to be unlocked
 * @folio: The folio to wait for.
 * @state: The sleep state (TASK_KILLABLE, TASK_UNINTERRUPTIBLE, etc).
 *
 * The caller should hold a reference on @folio.  They expect the page to
 * become unlocked relatively soon, but do not wish to hold up migration
 * (for example) by holding the reference while waiting for the folio to
 * come unlocked.  After this function returns, the caller should not
 * dereference @folio.
 *
 * Return: 0 if the folio was unlocked or -EINTR if interrupted by a signal.
 */
int folio_put_wait_locked(struct folio *folio, int state)
{
	return folio_wait_bit_common(folio, PG_locked, state, DROP);
}

/**
 * folio_unlock - Unlock a locked folio.
 * @folio: The folio.
 *
 * Unlocks the folio and wakes up any thread sleeping on the page lock.
 *
 * Context: May be called from interrupt or process context.  May not be
 * called from NMI context.
 */
void folio_unlock(struct folio *folio)
{
	/* Bit 7 allows x86 to check the byte's sign bit */
	BUILD_BUG_ON(PG_waiters != 7);
	BUILD_BUG_ON(PG_locked > 7);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	if (folio_xor_flags_has_waiters(folio, 1 << PG_locked))
		folio_wake_bit(folio, PG_locked);
}
EXPORT_SYMBOL(folio_unlock);

/**
 * folio_end_read - End read on a folio.
 * @folio: The folio.
 * @success: True if all reads completed successfully.
 *
 * When all reads against a folio have completed, filesystems should
 * call this function to let the pagecache know that no more reads
 * are outstanding.  This will unlock the folio and wake up any thread
 * sleeping on the lock.  The folio will also be marked uptodate if all
 * reads succeeded.
 *
 * Context: May be called from interrupt or process context.  May not be
 * called from NMI context.
 */
void folio_end_read(struct folio *folio, bool success)
{
	unsigned long mask = 1 << PG_locked;

	/* Must be in bottom byte for x86 to work */
	BUILD_BUG_ON(PG_uptodate > 7);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(success && folio_test_uptodate(folio), folio);

	if (likely(success))
		mask |= 1 << PG_uptodate;
	if (folio_xor_flags_has_waiters(folio, mask))
		folio_wake_bit(folio, PG_locked);
}
EXPORT_SYMBOL(folio_end_read);

/**
 * folio_end_private_2 - Clear PG_private_2 and wake any waiters.
 * @folio: The folio.
 *
 * Clear the PG_private_2 bit on a folio and wake up any sleepers waiting for
 * it.  The folio reference held for PG_private_2 being set is released.
 *
 * This is, for example, used when a netfs folio is being written to a local
 * disk cache, thereby allowing writes to the cache for the same folio to be
 * serialised.
 */
void folio_end_private_2(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_private_2(folio), folio);
	clear_bit_unlock(PG_private_2, folio_flags(folio, 0));
	folio_wake_bit(folio, PG_private_2);
	folio_put(folio);
}
EXPORT_SYMBOL(folio_end_private_2);

/**
 * folio_wait_private_2 - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 to be cleared on a folio.
 */
void folio_wait_private_2(struct folio *folio)
{
	while (folio_test_private_2(folio))
		folio_wait_bit(folio, PG_private_2);
}
EXPORT_SYMBOL(folio_wait_private_2);

/**
 * folio_wait_private_2_killable - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 to be cleared on a folio or until a fatal signal is
 * received by the calling task.
 *
 * Return:
 * - 0 if successful.
 * - -EINTR if a fatal signal was encountered.
 */
int folio_wait_private_2_killable(struct folio *folio)
{
	int ret = 0;

	while (folio_test_private_2(folio)) {
		ret = folio_wait_bit_killable(folio, PG_private_2);
		if (ret < 0)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(folio_wait_private_2_killable);

/**
 * __folio_lock - Get a lock on the folio, assuming we need to sleep to get it.
 * @folio: The folio to lock
 */
void __folio_lock(struct folio *folio)
{
	folio_wait_bit_common(folio, PG_locked, TASK_UNINTERRUPTIBLE,
				EXCLUSIVE);
}
EXPORT_SYMBOL(__folio_lock);

int __folio_lock_killable(struct folio *folio)
{
	return folio_wait_bit_common(folio, PG_locked, TASK_KILLABLE,
					EXCLUSIVE);
}
EXPORT_SYMBOL_GPL(__folio_lock_killable);

int __folio_lock_async(struct folio *folio, struct wait_page_queue *wait)
{
	struct wait_queue_head *q = folio_waitqueue(folio);
	int ret;

	wait->folio = folio;
	wait->bit_nr = PG_locked;

	spin_lock_irq(&q->lock);
	__add_wait_queue_entry_tail(q, &wait->wait);
	folio_set_waiters(folio);
	ret = !folio_trylock(folio);
	/*
	 * If we were successful now, we know we're still on the
	 * waitqueue as we're still under the lock. This means it's
	 * safe to remove and return success, we know the callback
	 * isn't going to trigger.
	 */
	if (!ret)
		__remove_wait_queue(q, &wait->wait);
	else
		ret = -EIOCBQUEUED;
	spin_unlock_irq(&q->lock);
	return ret;
}

/*
 * Return values:
 * 0 - folio is locked.
 * non-zero - folio is not locked.
 *     mmap_lock or per-VMA lock has been released (mmap_read_unlock() or
 *     vma_end_read()), unless flags had both FAULT_FLAG_ALLOW_RETRY and
 *     FAULT_FLAG_RETRY_NOWAIT set, in which case the lock is still held.
 *
 * If neither ALLOW_RETRY nor KILLABLE are set, will always return 0
 * with the folio locked and the mmap_lock/per-VMA lock is left unperturbed.
 */
vm_fault_t __folio_lock_or_retry(struct folio *folio, struct vm_fault *vmf)
{
	unsigned int flags = vmf->flags;

	if (fault_flag_allow_retry_first(flags)) {
		/*
		 * CAUTION! In this case, mmap_lock/per-VMA lock is not
		 * released even though returning VM_FAULT_RETRY.
		 */
		if (flags & FAULT_FLAG_RETRY_NOWAIT)
			return VM_FAULT_RETRY;

		release_fault_lock(vmf);
		if (flags & FAULT_FLAG_KILLABLE)
			folio_wait_locked_killable(folio);
		else
			folio_wait_locked(folio);
		return VM_FAULT_RETRY;
	}
	if (flags & FAULT_FLAG_KILLABLE) {
		bool ret;

		ret = __folio_lock_killable(folio);
		if (ret) {
			release_fault_lock(vmf);
			return VM_FAULT_RETRY;
		}
	} else {
		__folio_lock(folio);
	}

	return 0;
}
