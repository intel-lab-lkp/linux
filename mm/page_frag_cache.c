// SPDX-License-Identifier: GPL-2.0-only
/* Page fragment allocator
 *
 * Page Fragment:
 *  An arbitrary-length arbitrary-offset area of memory which resides within a
 *  0 or higher order page.  Multiple fragments within that page are
 *  individually refcounted, in the page's reference counter.
 *
 * The page_frag functions provide a simple allocation framework for page
 * fragments.  This is used by the network stack and network device drivers to
 * provide a backing region of memory for use as either an sk_buff->head, or to
 * be used in the "frags" portion of skb_shared_info.
 */

#include <linux/export.h>
#include <linux/gfp_types.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/page_frag_cache.h>
#include "internal.h"

static struct page *__page_frag_cache_reuse(unsigned long encoded_va,
					    unsigned int pagecnt_bias)
{
	struct page *page;

	page = virt_to_page((void *)encoded_va);
	if (!page_ref_sub_and_test(page, pagecnt_bias))
		return NULL;

	if (unlikely(encoded_page_pfmemalloc(encoded_va))) {
		free_unref_page(page, encoded_page_order(encoded_va));
		return NULL;
	}

	/* OK, page count is 0, we can safely set it */
	set_page_count(page, PAGE_FRAG_CACHE_MAX_SIZE + 1);
	return page;
}

static struct page *__page_frag_cache_refill(struct page_frag_cache *nc,
					     gfp_t gfp_mask)
{
	unsigned long order = PAGE_FRAG_CACHE_MAX_ORDER;
	struct page *page = NULL;
	gfp_t gfp = gfp_mask;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	gfp_mask = (gfp_mask & ~__GFP_DIRECT_RECLAIM) |  __GFP_COMP |
		   __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	page = __alloc_pages(gfp_mask, PAGE_FRAG_CACHE_MAX_ORDER,
			     numa_mem_id(), NULL);
#endif
	if (unlikely(!page)) {
		page = __alloc_pages(gfp, 0, numa_mem_id(), NULL);
		if (unlikely(!page)) {
			memset(nc, 0, sizeof(*nc));
			return NULL;
		}

		order = 0;
	}

	nc->encoded_va = encode_aligned_va(page_address(page), order,
					   page_is_pfmemalloc(page));

	/* Even if we own the page, we do not use atomic_set().
	 * This would break get_page_unless_zero() users.
	 */
	page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);

	return page;
}

/* Reload cache by reusing the old cache if it is possible, or
 * refilling from the page allocator.
 */
static struct page *__page_frag_cache_reload(struct page_frag_cache *nc,
					     gfp_t gfp_mask)
{
	struct page *page;

	if (likely(nc->encoded_va)) {
		page = __page_frag_cache_reuse(nc->encoded_va, nc->pagecnt_bias);
		if (page)
			goto out;
	}

	page = __page_frag_cache_refill(nc, gfp_mask);
	if (unlikely(!page))
		return NULL;

out:
	/* reset page count bias and remaining to start of new frag */
	nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
	nc->remaining = page_frag_cache_page_size(nc->encoded_va);
	return page;
}

/**
 * page_frag_alloc_va_prepare() - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @fragsz: in as the requested size, out as the available size
 * @gfp: the allocation gfp to use when cache needs to be refilled
 *
 * Prepare a page fragment with minimum size of @fragsz, @fragsz is also used
 * to report the maximum size of the page fragment the caller can use.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
void *page_frag_alloc_va_prepare(struct page_frag_cache *nc,
				 unsigned int *fragsz, gfp_t gfp)
{
	unsigned int remaining = nc->remaining;

	VM_BUG_ON(!*fragsz);
	if (likely(remaining >= *fragsz)) {
		unsigned long encoded_va = nc->encoded_va;

		*fragsz = remaining;

		return encoded_page_address(encoded_va) +
			(page_frag_cache_page_size(encoded_va) - remaining);
	}

	if (unlikely(*fragsz > PAGE_SIZE))
		return NULL;

	/* When reload fails, nc->encoded_va and nc->remaining are both reset
	 * to zero, so there is no need to check the return value here.
	 */
	__page_frag_cache_reload(nc, gfp);

	*fragsz = nc->remaining;
	return encoded_page_address(nc->encoded_va);
}
EXPORT_SYMBOL(page_frag_alloc_va_prepare);

/**
 * page_frag_alloc_pg_prepare - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @fragsz: in as the requested size, out as the available size
 * @gfp: the allocation gfp to use when cache needs to be refilled
 *
 * Prepare a page fragment with minimum size of @fragsz, @fragsz is also used
 * to report the maximum size of the page fragment the caller can use.
 *
 * Return:
 * the page fragment, otherwise return NULL.
 */
struct page *page_frag_alloc_pg_prepare(struct page_frag_cache *nc,
					unsigned int *offset,
					unsigned int *fragsz, gfp_t gfp)
{
	unsigned int remaining = nc->remaining;
	struct page *page;

	VM_BUG_ON(!*fragsz);
	if (likely(remaining >= *fragsz)) {
		unsigned long encoded_va = nc->encoded_va;

		*offset = page_frag_cache_page_size(encoded_va) - remaining;
		*fragsz = remaining;

		return virt_to_page((void *)encoded_va);
	}

	if (unlikely(*fragsz > PAGE_SIZE))
		return NULL;

	page = __page_frag_cache_reload(nc, gfp);
	*offset = 0;
	*fragsz = nc->remaining;
	return page;
}
EXPORT_SYMBOL(page_frag_alloc_pg_prepare);

/**
 * page_frag_alloc_prepare - Prepare allocing a page fragment.
 * @nc: page_frag cache from which to prepare
 * @offset: out as the offset of the page fragment
 * @fragsz: in as the requested size, out as the available size
 * @va: out as the virtual address of the returned page fragment
 * @gfp: the allocation gfp to use when cache needs to be refilled
 *
 * Prepare a page fragment with minimum size of @fragsz, @fragsz is also used
 * to report the maximum size of the page fragment. Return both 'struct page'
 * and virtual address of the fragment to the caller.
 *
 * Return:
 * the page fragment, otherwise return NULL.
 */
struct page *page_frag_alloc_prepare(struct page_frag_cache *nc,
				     unsigned int *offset,
				     unsigned int *fragsz,
				     void **va, gfp_t gfp)
{
	unsigned int remaining = nc->remaining;
	struct page *page;

	VM_BUG_ON(!*fragsz);
	if (likely(remaining >= *fragsz)) {
		unsigned long encoded_va = nc->encoded_va;

		*offset = page_frag_cache_page_size(encoded_va) - remaining;
		*va = encoded_page_address(encoded_va) + *offset;
		*fragsz = remaining;

		return virt_to_page((void *)encoded_va);
	}

	if (unlikely(*fragsz > PAGE_SIZE))
		return NULL;

	page = __page_frag_cache_reload(nc, gfp);
	*offset = 0;
	*fragsz = nc->remaining;
	*va = encoded_page_address(nc->encoded_va);

	return page;
}
EXPORT_SYMBOL(page_frag_alloc_prepare);

/**
 * page_frag_alloc_pg - Alloce a page fragment.
 * @nc: page_frag cache from which to alloce
 * @offset: out as the offset of the page fragment
 * @fragsz: the requested fragment size
 * @gfp: the allocation gfp to use when cache needs to be refilled
 *
 * Get a page fragment from page_frag cache.
 *
 * Return:
 * the page fragment, otherwise return NULL.
 */
struct page *page_frag_alloc_pg(struct page_frag_cache *nc,
				unsigned int *offset, unsigned int fragsz,
				gfp_t gfp)
{
	unsigned int remaining = nc->remaining;
	struct page *page;

	VM_BUG_ON(!fragsz);
	if (likely(remaining >= fragsz)) {
		unsigned long encoded_va = nc->encoded_va;

		*offset = page_frag_cache_page_size(encoded_va) -
				remaining;

		return virt_to_page((void *)encoded_va);
	}

	if (unlikely(fragsz > PAGE_SIZE))
		return NULL;

	page = __page_frag_cache_reload(nc, gfp);
	if (unlikely(!page))
		return NULL;

	*offset = 0;
	nc->remaining = remaining - fragsz;
	nc->pagecnt_bias--;

	return page;
}
EXPORT_SYMBOL(page_frag_alloc_pg);

/**
 * page_frag_cache_drain - Drain the current page from page_frag cache.
 * @nc: page_frag cache from which to drain
 */
void page_frag_cache_drain(struct page_frag_cache *nc)
{
	if (!nc->encoded_va)
		return;

	__page_frag_cache_drain(virt_to_head_page((void *)nc->encoded_va),
				nc->pagecnt_bias);
	memset(nc, 0, sizeof(*nc));
}
EXPORT_SYMBOL(page_frag_cache_drain);

void __page_frag_cache_drain(struct page *page, unsigned int count)
{
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);

	if (page_ref_sub_and_test(page, count))
		free_unref_page(page, compound_order(page));
}
EXPORT_SYMBOL(__page_frag_cache_drain);

/**
 * __page_frag_alloc_va_align() - Alloc a page fragment with aligning
 * requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the 'va'
 *
 * Get a page fragment from page_frag cache with aligning requirement.
 *
 * Return:
 * Return va of the page fragment, otherwise return NULL.
 */
void *__page_frag_alloc_va_align(struct page_frag_cache *nc,
				 unsigned int fragsz, gfp_t gfp_mask,
				 unsigned int align_mask)
{
	unsigned long encoded_va = nc->encoded_va;
	unsigned int remaining;

	remaining = nc->remaining & align_mask;

	/* As we have ensured remaining is zero when initiating and draining old
	 * cache, 'remaining >= fragsz' checking is enough to indicate there is
	 * enough available space for the new fragment allocation.
	 */
	if (likely(remaining >= fragsz)) {
		nc->pagecnt_bias--;
		nc->remaining = remaining - fragsz;

		return encoded_page_address(encoded_va) +
			(page_frag_cache_page_size(encoded_va) - remaining);
	}

	if (unlikely(fragsz > PAGE_SIZE)) {
		/*
		 * The caller is trying to allocate a fragment with
		 * fragsz > PAGE_SIZE but the cache isn't big enough to satisfy
		 * the request, this may happen in low memory conditions. We don't
		 * release the cache page because it could make memory pressure
		 * worse so we simply return NULL here.
		 */
		return NULL;
	}

	if (unlikely(!__page_frag_cache_reload(nc, gfp_mask)))
		return NULL;

	/* As the we are allocating fragment from cache by count-up way, the offset
	 * of allocated fragment from the just reloaded cache is zero, so remaining
	 * aligning and offset calculation are not needed.
	 */
	nc->pagecnt_bias--;
	nc->remaining -= fragsz;

	return encoded_page_address(nc->encoded_va);
}
EXPORT_SYMBOL(__page_frag_alloc_va_align);

/**
 * page_frag_free_va - Free a page fragment.
 * @addr: va of page fragment to be freed
 *
 * Free a page fragment allocated out of either a compound or order 0 page by
 * virtual address.
 */
void page_frag_free_va(void *addr)
{
	struct page *page = virt_to_head_page(addr);

	if (unlikely(put_page_testzero(page)))
		free_unref_page(page, compound_order(page));
}
EXPORT_SYMBOL(page_frag_free_va);
