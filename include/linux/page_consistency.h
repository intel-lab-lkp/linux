/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Dual-bitmap page consistency checking
 *
 * Provides corruption detection for page allocations using complementary
 * bitmaps where the invariant (primary == ~secondary) must hold.
 *
 * Based on NVIDIA safety research.
 */
#ifndef _LINUX_PAGE_CONSISTENCY_H
#define _LINUX_PAGE_CONSISTENCY_H

#include <linux/types.h>
#include <linux/mm_types.h>

/* Return codes for page consistency checking */
enum page_consistency_result {
	PAGE_CONSISTENCY_OK = 0,
	PAGE_CONSISTENCY_MISMATCH,
	PAGE_CONSISTENCY_NOT_TRACKED,
};

#ifdef CONFIG_DEBUG_PAGE_CONSISTENCY

#include <linux/jump_label.h>
DECLARE_STATIC_KEY_FALSE(page_consistency_enabled);

/* Initialization - called during mm_core_init() */
void __init page_consistency_init(void);

/* Core tracking functions */
void __page_consistency_alloc(struct page *page, unsigned int order);
void __page_consistency_free(struct page *page, unsigned int order);

/* Validation functions */
enum page_consistency_result page_consistency_check_page(struct page *page);
enum page_consistency_result page_consistency_validate_all(void);

/**
 * page_consistency_alloc - Track page allocation
 * @page: Allocated page
 * @order: Allocation order
 *
 * Called from post_alloc_hook() to track page allocations.
 * The static key avoids the out-of-line tracking call until initialization.
 */
static inline void page_consistency_alloc(struct page *page, unsigned int order)
{
	if (static_branch_unlikely(&page_consistency_enabled))
		__page_consistency_alloc(page, order);
}

/**
 * page_consistency_free - Track page free
 * @page: Page being freed
 * @order: Free order
 *
 * Called from free_pages_prepare() to track page frees.
 * The static key avoids the out-of-line tracking call until initialization.
 */
static inline void page_consistency_free(struct page *page, unsigned int order)
{
	if (static_branch_unlikely(&page_consistency_enabled))
		__page_consistency_free(page, order);
}

#else /* !CONFIG_DEBUG_PAGE_CONSISTENCY */

static inline void __init page_consistency_init(void) {}
static inline void page_consistency_alloc(struct page *page, unsigned int order) {}
static inline void page_consistency_free(struct page *page, unsigned int order) {}

static inline enum page_consistency_result page_consistency_check_page(struct page *page)
{
	return PAGE_CONSISTENCY_OK;
}

static inline enum page_consistency_result page_consistency_validate_all(void)
{
	return PAGE_CONSISTENCY_OK;
}

#endif /* CONFIG_DEBUG_PAGE_CONSISTENCY */
#endif /* _LINUX_PAGE_CONSISTENCY_H */
