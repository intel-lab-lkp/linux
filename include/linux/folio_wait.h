/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FOLIO_WAIT_H
#define _LINUX_FOLIO_WAIT_H

#include <linux/bitops.h>
#include <linux/page-flags.h>
#include <linux/wait.h>

struct wait_folio_key {
	struct folio *folio;
	int bit_nr;
	int folio_match;
};

struct wait_folio_queue {
	struct folio *folio;
	int bit_nr;
	wait_queue_entry_t wait;
};

static inline bool wake_folio_match(struct wait_folio_queue *wait_folio,
		struct wait_folio_key *key)
{
	if (wait_folio->folio != key->folio)
		return false;
	key->folio_match = 1;

	if (wait_folio->bit_nr != key->bit_nr)
		return false;

	return true;
}

void __folio_lock(struct folio *folio);
int __folio_lock_killable(struct folio *folio);
vm_fault_t __folio_lock_or_retry(struct folio *folio, struct vm_fault *vmf);
void unlock_page(struct page *page);
void folio_unlock(struct folio *folio);

/**
 * folio_trylock() - Attempt to lock a folio.
 * @folio: The folio to attempt to lock.
 *
 * Sometimes it is undesirable to wait for a folio to be unlocked (e.g. when
 * the locks are being taken in the wrong order, or if making progress through
 * a batch of folios is more important than processing them in order). Usually
 * folio_lock() is the correct function to call.
 *
 * Context: Any context.
 * Return: Whether the lock was successfully acquired.
 */
static inline bool folio_trylock(struct folio *folio)
{
	return likely(!test_and_set_bit_lock(PG_locked, folio_flags(folio, 0)));
}

/*
 * Return true if the page was successfully locked
 */
static inline bool trylock_page(struct page *page)
{
	return folio_trylock(page_folio(page));
}

/**
 * folio_lock() - Lock this folio.
 * @folio: The folio to lock.
 *
 * The folio lock protects against many things, probably more than it should.
 * It is primarily held while a folio is being brought uptodate, either from
 * its backing file or from swap. It is also held while a folio is being
 * truncated from its address_space, so holding the lock is sufficient to keep
 * folio->mapping stable.
 *
 * The folio lock is also held while write() is modifying the folio to provide
 * POSIX atomicity guarantees (as long as the write does not cross a page
 * boundary). Other modifications to the data in the folio do not hold the
 * folio lock and can race with writes, e.g. DMA and stores to mapped pages.
 *
 * Context: May sleep. If you need to acquire the locks of two or more folios,
 * they must be in order of ascending index, if they are in the same
 * address_space. If they are in different address_spaces, acquire the lock of
 * the folio which belongs to the address_space which has the lowest address in
 * memory first.
 */
static inline void folio_lock(struct folio *folio)
{
	might_sleep();
	if (!folio_trylock(folio))
		__folio_lock(folio);
}

/**
 * lock_page() - Lock the folio containing this page.
 * @page: The page to lock.
 *
 * See folio_lock() for a description of what the lock protects.
 * This is a legacy function and new code should probably use folio_lock()
 * instead.
 *
 * Context: May sleep. Pages in the same folio share a lock, so do not attempt
 * to lock two pages which share a folio.
 */
static inline void lock_page(struct page *page)
{
	struct folio *folio;
	might_sleep();

	folio = page_folio(page);
	if (!folio_trylock(folio))
		__folio_lock(folio);
}

/**
 * folio_lock_killable() - Lock this folio, interruptible by a fatal signal.
 * @folio: The folio to lock.
 *
 * Attempts to lock the folio, like folio_lock(), except that the sleep to
 * acquire the lock is interruptible by a fatal signal.
 *
 * Context: May sleep; see folio_lock().
 * Return: 0 if the lock was acquired; -EINTR if a fatal signal was received.
 */
static inline int folio_lock_killable(struct folio *folio)
{
	might_sleep();
	if (!folio_trylock(folio))
		return __folio_lock_killable(folio);
	return 0;
}

/*
 * folio_lock_or_retry - Lock the folio, unless this would block and the caller
 * indicated that it can handle a retry.
 *
 * Return value and mmap_lock implications depend on flags; see
 * __folio_lock_or_retry().
 */
static inline vm_fault_t folio_lock_or_retry(struct folio *folio,
					     struct vm_fault *vmf)
{
	might_sleep();
	if (!folio_trylock(folio))
		return __folio_lock_or_retry(folio, vmf);
	return 0;
}

/*
 * This is exported only for folio_wait_locked/folio_wait_writeback, etc., and
 * should not be used directly.
 */
void folio_wait_bit(struct folio *folio, int bit_nr);
int folio_wait_bit_killable(struct folio *folio, int bit_nr);

/*
 * Wait for a folio to be unlocked.
 *
 * This must be called with the caller "holding" the folio, i.e. with increased
 * folio reference count so that the folio won't go away during the wait.
 */
static inline void folio_wait_locked(struct folio *folio)
{
	if (folio_test_locked(folio))
		folio_wait_bit(folio, PG_locked);
}

static inline int folio_wait_locked_killable(struct folio *folio)
{
	if (!folio_test_locked(folio))
		return 0;
	return folio_wait_bit_killable(folio, PG_locked);
}

void folio_end_read(struct folio *folio, bool success);
void folio_end_private_2(struct folio *folio);
void folio_wait_private_2(struct folio *folio);
int folio_wait_private_2_killable(struct folio *folio);

void folio_wait_writeback(struct folio *folio);
int folio_wait_writeback_killable(struct folio *folio);
void folio_wait_stable(struct folio *folio);

#endif /* _LINUX_FOLIO_WAIT_H */
