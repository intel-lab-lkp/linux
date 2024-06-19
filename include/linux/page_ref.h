/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_REF_H
#define _LINUX_PAGE_REF_H

#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/tracepoint-defs.h>

DECLARE_TRACEPOINT(page_ref_set);
DECLARE_TRACEPOINT(page_ref_mod);
DECLARE_TRACEPOINT(page_ref_mod_and_test);
DECLARE_TRACEPOINT(page_ref_mod_and_return);
DECLARE_TRACEPOINT(page_ref_mod_unless);
DECLARE_TRACEPOINT(page_ref_freeze);
DECLARE_TRACEPOINT(page_ref_unfreeze);

#ifdef CONFIG_DEBUG_PAGE_REF

/*
 * Ideally we would want to use the trace_<tracepoint>_enabled() helper
 * functions. But due to include header file issues, that is not
 * feasible. Instead we have to open code the static key functions.
 *
 * See trace_##name##_enabled(void) in include/linux/tracepoint.h
 */
#define page_ref_tracepoint_active(t) tracepoint_enabled(t)

extern void __page_ref_set(struct page *page, int v);
extern void __page_ref_mod(struct page *page, int v);
extern void __page_ref_mod_and_test(struct page *page, int v, int ret);
extern void __page_ref_mod_and_return(struct page *page, int v, int ret);
extern void __page_ref_mod_unless(struct page *page, int v, int u);
extern void __page_ref_freeze(struct page *page, int v, int ret);
extern void __page_ref_unfreeze(struct page *page, int v);

#else

#define page_ref_tracepoint_active(t) false

static inline void __page_ref_set(struct page *page, int v)
{
}
static inline void __page_ref_mod(struct page *page, int v)
{
}
static inline void __page_ref_mod_and_test(struct page *page, int v, int ret)
{
}
static inline void __page_ref_mod_and_return(struct page *page, int v, int ret)
{
}
static inline void __page_ref_mod_unless(struct page *page, int v, int u)
{
}
static inline void __page_ref_freeze(struct page *page, int v, int ret)
{
}
static inline void __page_ref_unfreeze(struct page *page, int v)
{
}

#endif

/*
 * GUP_PIN_COUNTING_BIAS, and the associated functions that use it, overload
 * the page's refcount so that two separate items are tracked: the original page
 * reference count, and also a new count of how many pin_user_pages() calls were
 * made against the page. ("gup-pinned" is another term for the latter).
 *
 * With this scheme, pin_user_pages() becomes special: such pages are marked as
 * distinct from normal pages. As such, the unpin_user_page() call (and its
 * variants) must be used in order to release gup-pinned pages.
 *
 * Choice of value:
 *
 * By making GUP_PIN_COUNTING_BIAS a power of two, debugging of page reference
 * counts with respect to pin_user_pages() and unpin_user_page() becomes
 * simpler, due to the fact that adding an even power of two to the page
 * refcount has the effect of using only the upper N bits, for the code that
 * counts up using the bias value. This means that the lower bits are left for
 * the exclusive use of the original code that increments and decrements by one
 * (or at least, by much smaller values than the bias value).
 *
 * Of course, once the lower bits overflow into the upper bits (and this is
 * OK, because subtraction recovers the original values), then visual inspection
 * no longer suffices to directly view the separate counts. However, for normal
 * applications that don't have huge page reference counts, this won't be an
 * issue.
 *
 * Locking: the lockless algorithm described in folio_try_get_rcu()
 * provides safe operation for get_user_pages(), page_mkclean() and
 * other calls that race to set up page table entries.
 */
#define GUP_PIN_COUNTING_BIAS (1U << 10)

/*
 * GUP_PIN_EXCLUSIVE_BIAS is used to grab an exclusive pin over a page.
 * This exclusive pin can only be taken once, and only if no other GUP pins
 * exist for the page.
 * After it's taken, no other gup pins can be taken.
 */
#define GUP_PIN_EXCLUSIVE_BIAS (1U << 30)

static inline int page_ref_count(const struct page *page)
{
	return atomic_read(&page->_refcount);
}

/**
 * folio_ref_count - The reference count on this folio.
 * @folio: The folio.
 *
 * The refcount is usually incremented by calls to folio_get() and
 * decremented by calls to folio_put().  Some typical users of the
 * folio refcount:
 *
 * - Each reference from a page table
 * - The page cache
 * - Filesystem private data
 * - The LRU list
 * - Pipes
 * - Direct IO which references this page in the process address space
 *
 * Return: The number of references to this folio.
 */
static inline int folio_ref_count(const struct folio *folio)
{
	return page_ref_count(&folio->page);
}

static inline int page_count(const struct page *page)
{
	return folio_ref_count(page_folio(page));
}

static inline void set_page_count(struct page *page, int v)
{
	atomic_set(&page->_refcount, v);
	if (page_ref_tracepoint_active(page_ref_set))
		__page_ref_set(page, v);
}

static inline void folio_set_count(struct folio *folio, int v)
{
	set_page_count(&folio->page, v);
}

/*
 * Setup the page count before being freed into the page allocator for
 * the first time (boot or memory hotplug)
 */
static inline void init_page_count(struct page *page)
{
	set_page_count(page, 1);
}

static __must_check inline bool page_ref_setexc(struct page *page, unsigned int refs)
{
	unsigned int old_count, new_count;

	if (WARN_ON_ONCE(refs >= GUP_PIN_EXCLUSIVE_BIAS))
		return false;

	do {
		old_count = atomic_read(&page->_refcount);

		if (old_count >= GUP_PIN_COUNTING_BIAS)
			return false;

		if (check_add_overflow(old_count, refs + GUP_PIN_EXCLUSIVE_BIAS, &new_count))
			return false;
	} while (atomic_cmpxchg(&page->_refcount, old_count, new_count) != old_count);

	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, refs);

	return true;
}

static __must_check inline bool folio_ref_setexc(struct folio *folio, unsigned int refs)
{
	return page_ref_setexc(&folio->page, refs);
}

static inline void page_ref_add(struct page *page, int nr)
{
	atomic_add(nr, &page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, nr);
}

static inline void folio_ref_add(struct folio *folio, int nr)
{
	page_ref_add(&folio->page, nr);
}

static inline void page_ref_sub(struct page *page, int nr)
{
	atomic_sub(nr, &page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, -nr);
}

static inline void folio_ref_sub(struct folio *folio, int nr)
{
	page_ref_sub(&folio->page, nr);
}

static inline int folio_ref_sub_return(struct folio *folio, int nr)
{
	int ret = atomic_sub_return(nr, &folio->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_return))
		__page_ref_mod_and_return(&folio->page, -nr, ret);
	return ret;
}

static inline void page_ref_inc(struct page *page)
{
	atomic_inc(&page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, 1);
}

static inline void folio_ref_inc(struct folio *folio)
{
	page_ref_inc(&folio->page);
}

static inline void page_ref_dec(struct page *page)
{
	atomic_dec(&page->_refcount);
	if (page_ref_tracepoint_active(page_ref_mod))
		__page_ref_mod(page, -1);
}

static inline void folio_ref_dec(struct folio *folio)
{
	page_ref_dec(&folio->page);
}

static inline int page_ref_sub_and_test(struct page *page, int nr)
{
	int ret = atomic_sub_and_test(nr, &page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_test))
		__page_ref_mod_and_test(page, -nr, ret);
	return ret;
}

static inline int folio_ref_sub_and_test(struct folio *folio, int nr)
{
	return page_ref_sub_and_test(&folio->page, nr);
}

static inline int page_ref_inc_return(struct page *page)
{
	int ret = atomic_inc_return(&page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_return))
		__page_ref_mod_and_return(page, 1, ret);
	return ret;
}

static inline int folio_ref_inc_return(struct folio *folio)
{
	return page_ref_inc_return(&folio->page);
}

static inline int page_ref_dec_and_test(struct page *page)
{
	int ret = atomic_dec_and_test(&page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_test))
		__page_ref_mod_and_test(page, -1, ret);
	return ret;
}

static inline int folio_ref_dec_and_test(struct folio *folio)
{
	return page_ref_dec_and_test(&folio->page);
}

static inline int page_ref_dec_return(struct page *page)
{
	int ret = atomic_dec_return(&page->_refcount);

	if (page_ref_tracepoint_active(page_ref_mod_and_return))
		__page_ref_mod_and_return(page, -1, ret);
	return ret;
}

static inline int folio_ref_dec_return(struct folio *folio)
{
	return page_ref_dec_return(&folio->page);
}

static inline bool page_ref_add_unless(struct page *page, int nr, int u)
{
	bool ret = atomic_add_unless(&page->_refcount, nr, u);

	if (page_ref_tracepoint_active(page_ref_mod_unless))
		__page_ref_mod_unless(page, nr, ret);
	return ret;
}

static inline bool folio_ref_add_unless(struct folio *folio, int nr, int u)
{
	return page_ref_add_unless(&folio->page, nr, u);
}

/**
 * folio_try_get - Attempt to increase the refcount on a folio.
 * @folio: The folio.
 *
 * If you do not already have a reference to a folio, you can attempt to
 * get one using this function.  It may fail if, for example, the folio
 * has been freed since you found a pointer to it, or it is frozen for
 * the purposes of splitting or migration.
 *
 * Return: True if the reference count was successfully incremented.
 */
static inline bool folio_try_get(struct folio *folio)
{
	return folio_ref_add_unless(folio, 1, 0);
}

static inline bool folio_ref_try_add_rcu(struct folio *folio, int count)
{
#ifdef CONFIG_TINY_RCU
	/*
	 * The caller guarantees the folio will not be freed from interrupt
	 * context, so (on !SMP) we only need preemption to be disabled
	 * and TINY_RCU does that for us.
	 */
# ifdef CONFIG_PREEMPT_COUNT
	VM_BUG_ON(!in_atomic() && !irqs_disabled());
# endif
	VM_BUG_ON_FOLIO(folio_ref_count(folio) == 0, folio);
	folio_ref_add(folio, count);
#else
	if (unlikely(!folio_ref_add_unless(folio, count, 0))) {
		/* Either the folio has been freed, or will be freed. */
		return false;
	}
#endif
	return true;
}

/**
 * folio_try_get_rcu - Attempt to increase the refcount on a folio.
 * @folio: The folio.
 *
 * This is a version of folio_try_get() optimised for non-SMP kernels.
 * If you are still holding the rcu_read_lock() after looking up the
 * page and know that the page cannot have its refcount decreased to
 * zero in interrupt context, you can use this instead of folio_try_get().
 *
 * Example users include get_user_pages_fast() (as pages are not unmapped
 * from interrupt context) and the page cache lookups (as pages are not
 * truncated from interrupt context).  We also know that pages are not
 * frozen in interrupt context for the purposes of splitting or migration.
 *
 * You can also use this function if you're holding a lock that prevents
 * pages being frozen & removed; eg the i_pages lock for the page cache
 * or the mmap_lock or page table lock for page tables.  In this case,
 * it will always succeed, and you could have used a plain folio_get(),
 * but it's sometimes more convenient to have a common function called
 * from both locked and RCU-protected contexts.
 *
 * Return: True if the reference count was successfully incremented.
 */
static inline bool folio_try_get_rcu(struct folio *folio)
{
	return folio_ref_try_add_rcu(folio, 1);
}

static inline int page_ref_freeze(struct page *page, int count)
{
	int ret = likely(atomic_cmpxchg(&page->_refcount, count, 0) == count);

	if (page_ref_tracepoint_active(page_ref_freeze))
		__page_ref_freeze(page, count, ret);
	return ret;
}

static inline int folio_ref_freeze(struct folio *folio, int count)
{
	return page_ref_freeze(&folio->page, count);
}

static inline void page_ref_unfreeze(struct page *page, int count)
{
	VM_BUG_ON_PAGE(page_count(page) != 0, page);
	VM_BUG_ON(count == 0);

	atomic_set_release(&page->_refcount, count);
	if (page_ref_tracepoint_active(page_ref_unfreeze))
		__page_ref_unfreeze(page, count);
}

static inline void folio_ref_unfreeze(struct folio *folio, int count)
{
	page_ref_unfreeze(&folio->page, count);
}
#endif
