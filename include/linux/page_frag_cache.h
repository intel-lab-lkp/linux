/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/bits.h>
#include <linux/log2.h>
#include <linux/mmdebug.h>
#include <linux/mm_types_task.h>
#include <linux/types.h>

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
/* Use a full byte here to enable assembler optimization as the shift
 * operation is usually expecting a byte.
 */
#define PAGE_FRAG_CACHE_ORDER_MASK		GENMASK(7, 0)
#else
/* Compiler should be able to figure out we don't read things as any value
 * ANDed with 0 is 0.
 */
#define PAGE_FRAG_CACHE_ORDER_MASK		0
#endif

#define PAGE_FRAG_CACHE_PFMEMALLOC_BIT		(PAGE_FRAG_CACHE_ORDER_MASK + 1)

static inline bool encoded_page_decode_pfmemalloc(unsigned long encoded_page)
{
	return !!(encoded_page & PAGE_FRAG_CACHE_PFMEMALLOC_BIT);
}

/**
 * page_frag_cache_init() - Init page_frag cache.
 * @nc: page_frag cache from which to init
 *
 * Inline helper to initialize the page_frag cache.
 */
static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->encoded_page = 0;
}

/**
 * page_frag_cache_is_pfmemalloc() - Check for pfmemalloc.
 * @nc: page_frag cache from which to check
 *
 * Check if the current page in page_frag cache is allocated from the pfmemalloc
 * reserves. It has the same calling context expectation as the allocation API.
 *
 * Return:
 * true if the current page in page_frag cache is allocated from the pfmemalloc
 * reserves, otherwise return false.
 */
static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return encoded_page_decode_pfmemalloc(nc->encoded_page);
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);
void *__page_frag_cache_prepare(struct page_frag_cache *nc, unsigned int fragsz,
				struct page_frag *pfrag, gfp_t gfp_mask,
				unsigned int align_mask);
unsigned int __page_frag_cache_commit_noref(struct page_frag_cache *nc,
					    struct page_frag *pfrag,
					    unsigned int used_sz);

static inline unsigned int __page_frag_cache_commit(struct page_frag_cache *nc,
						    struct page_frag *pfrag,
						    unsigned int used_sz)
{
	VM_BUG_ON(!nc->pagecnt_bias);
	nc->pagecnt_bias--;

	return __page_frag_cache_commit_noref(nc, pfrag, used_sz);
}

/**
 * __page_frag_alloc_align() - Allocate a page fragment with aligning
 * requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the 'va'
 *
 * Allocate a page fragment from page_frag cache with aligning requirement.
 *
 * Return:
 * Virtual address of the page fragment, otherwise return NULL.
 */
static inline void *__page_frag_alloc_align(struct page_frag_cache *nc,
					    unsigned int fragsz, gfp_t gfp_mask,
					    unsigned int align_mask)
{
	struct page_frag page_frag;
	void *va;

	va = __page_frag_cache_prepare(nc, fragsz, &page_frag, gfp_mask,
				       align_mask);
	if (likely(va))
		__page_frag_cache_commit(nc, &page_frag, fragsz);

	return va;
}

/**
 * page_frag_alloc_align() - Allocate a page fragment with aligning requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache needs to be refilled
 * @align: the requested aligning requirement for the fragment
 *
 * WARN_ON_ONCE() checking for @align before allocating a page fragment from
 * page_frag cache with aligning requirement.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_align(struct page_frag_cache *nc,
					  unsigned int fragsz, gfp_t gfp_mask,
					  unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, -align);
}

/**
 * page_frag_alloc() - Allocate a page fragment.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Allocate a page fragment from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc(struct page_frag_cache *nc,
				    unsigned int fragsz, gfp_t gfp_mask)
{
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, ~0u);
}

void page_frag_free(void *addr);

#endif
