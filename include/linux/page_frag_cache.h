/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_FRAG_CACHE_H
#define _LINUX_PAGE_FRAG_CACHE_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/mmdebug.h>
#include <linux/mm_types_task.h>
#include <linux/types.h>

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
/* Use a full byte here to enable assembler optimization as the shift
 * operation is usually expecting a byte.
 */
#define PAGE_FRAG_CACHE_ORDER_MASK		GENMASK(7, 0)
#define PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT	8
#define PAGE_FRAG_CACHE_PFMEMALLOC_BIT		BIT(PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT)
#else
/* Compiler should be able to figure out we don't read things as any value
 * ANDed with 0 is 0.
 */
#define PAGE_FRAG_CACHE_ORDER_MASK		0
#define PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT	0
#define PAGE_FRAG_CACHE_PFMEMALLOC_BIT		BIT(PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT)
#endif

static inline unsigned long page_frag_encode_page(struct page *page,
						  unsigned int order,
						  bool pfmemalloc)
{
	BUILD_BUG_ON(PAGE_FRAG_CACHE_MAX_ORDER > PAGE_FRAG_CACHE_ORDER_MASK);
	BUILD_BUG_ON(PAGE_FRAG_CACHE_PFMEMALLOC_BIT >= PAGE_SIZE);

	return (unsigned long)page_address(page) |
		(order & PAGE_FRAG_CACHE_ORDER_MASK) |
		((unsigned long)pfmemalloc << PAGE_FRAG_CACHE_PFMEMALLOC_SHIFT);
}

static inline unsigned long page_frag_encoded_page_order(unsigned long encoded_page)
{
	return encoded_page & PAGE_FRAG_CACHE_ORDER_MASK;
}

static inline bool page_frag_encoded_page_pfmemalloc(unsigned long encoded_page)
{
	return !!(encoded_page & PAGE_FRAG_CACHE_PFMEMALLOC_BIT);
}

static inline void *page_frag_encoded_page_address(unsigned long encoded_page)
{
	return (void *)(encoded_page & PAGE_MASK);
}

static inline struct page *page_frag_encoded_page_ptr(unsigned long encoded_page)
{
	return virt_to_page((void *)encoded_page);
}

/**
 * page_frag_cache_init() - Init page_frag cache.
 * @nc: page_frag cache from which to init
 *
 * Inline helper to init the page_frag cache.
 */
static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->encoded_page = 0;
}

/**
 * page_frag_cache_is_pfmemalloc() - Check for pfmemalloc.
 * @nc: page_frag cache from which to check
 *
 * Used to check if the current page in page_frag cache is pfmemalloc'ed.
 * It has the same calling context expectation as the alloc API.
 *
 * Return:
 * true if the current page in page_frag cache is pfmemalloc'ed, otherwise
 * return false.
 */
static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return page_frag_encoded_page_pfmemalloc(nc->encoded_page);
}

static inline unsigned int page_frag_cache_page_size(unsigned long encoded_page)
{
	return PAGE_SIZE << page_frag_encoded_page_order(encoded_page);
}

/**
 * page_frag_cache_page_offset() - Return the current page fragment's offset.
 * @nc: page_frag cache from which to check
 *
 * The API is only used in net/sched/em_meta.c for historical reason, do not use
 * it for new caller unless there is a strong reason.
 *
 * Return:
 * the offset of the current page fragment in the page_frag cache.
 */
static inline unsigned int page_frag_cache_page_offset(const struct page_frag_cache *nc)
{
	return nc->offset;
}

void page_frag_cache_drain(struct page_frag_cache *nc);
void __page_frag_cache_drain(struct page *page, unsigned int count);
void *__page_frag_cache_prepare(struct page_frag_cache *nc, unsigned int fragsz,
				struct page_frag *pfrag, gfp_t gfp_mask,
				unsigned int align_mask);

static inline void __page_frag_cache_commit(struct page_frag_cache *nc,
					    struct page_frag *pfrag, bool referenced,
					    unsigned int used_sz)
{
	if (referenced) {
		VM_BUG_ON(!nc->pagecnt_bias);
		nc->pagecnt_bias--;
	}

	VM_BUG_ON(used_sz > pfrag->size);
	VM_BUG_ON(pfrag->page != page_frag_encoded_page_ptr(nc->encoded_page));

	/* nc->offset is not reset when reusing an old page, so do not check for the
	 * first fragment.
	 * Committed offset might be bigger than the current offset due to alignment
	 */
	VM_BUG_ON(pfrag->offset && nc->offset > pfrag->offset);
	VM_BUG_ON(pfrag->offset &&
		  pfrag->offset + pfrag->size > page_frag_cache_page_size(nc->encoded_page));

	pfrag->size = used_sz;

	/* Calculate true size for the fragment due to alignment, nc->offset is not
	 * reset for the first fragment when reusing an old page.
	 */
	pfrag->size += pfrag->offset ? (pfrag->offset - nc->offset) : 0;

	nc->offset = pfrag->offset + used_sz;
}

/**
 * __page_frag_alloc_align() - Alloc a page fragment with aligning
 * requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the 'va'
 *
 * Alloc a page fragment from page_frag cache with aligning requirement.
 *
 * Return:
 * Virtual address of the page fragment, otherwise return NULL.
 */
static inline void *__page_frag_alloc_align(struct page_frag_cache *nc, unsigned int fragsz,
					    gfp_t gfp_mask, unsigned int align_mask)
{
	struct page_frag page_frag;
	void *va;

	va = __page_frag_cache_prepare(nc, fragsz, &page_frag, gfp_mask, align_mask);
	if (unlikely(!va))
		return NULL;

	__page_frag_cache_commit(nc, &page_frag, true, fragsz);

	return va;
}

/**
 * page_frag_alloc_align() - Alloc a page fragment with aligning requirement.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache needs to be refilled
 * @align: the requested aligning requirement for the fragment
 *
 * WARN_ON_ONCE() checking for @align before allocing a page fragment from
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
 * page_frag_alloc() - Alloc a page fragment.
 * @nc: page_frag cache from which to allocate
 * @fragsz: the requested fragment size
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Alloc a page fragment from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc(struct page_frag_cache *nc,
				    unsigned int fragsz, gfp_t gfp_mask)
{
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, ~0u);
}

/**
 * __page_frag_refill_align() - Refill a page_frag with aligning requirement.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the fragment
 *
 * Refill a page_frag from page_frag cache with aligning requirement.
 *
 * Return:
 * True if refill succeeds, otherwise return false.
 */
static inline bool __page_frag_refill_align(struct page_frag_cache *nc, unsigned int fragsz,
					    struct page_frag *pfrag, gfp_t gfp_mask,
					    unsigned int align_mask)
{
	if (unlikely(!__page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask, align_mask)))
		return false;

	__page_frag_cache_commit(nc, pfrag, true, fragsz);
	return true;
}

/**
 * page_frag_refill_align() - Refill a page_frag with aligning requirement.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache needs to be refilled
 * @align: the requested aligning requirement for the fragment
 *
 * WARN_ON_ONCE() checking for @align before refilling a page_frag from
 * page_frag cache with aligning requirement.
 *
 * Return:
 * True if refill succeeds, otherwise return false.
 */
static inline bool page_frag_refill_align(struct page_frag_cache *nc, unsigned int fragsz,
					  struct page_frag *pfrag, gfp_t gfp_mask,
					  unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_refill_align(nc, fragsz, pfrag, gfp_mask, -align);
}

/**
 * page_frag_refill() - Refill a page_frag.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Refill a page_frag from page_frag cache.
 *
 * Return:
 * True if refill succeeds, otherwise return false.
 */
static inline bool page_frag_refill(struct page_frag_cache *nc, unsigned int fragsz,
				    struct page_frag *pfrag, gfp_t gfp_mask)
{
	return __page_frag_refill_align(nc, fragsz, pfrag, gfp_mask, ~0u);
}

/**
 * __page_frag_refill_prepare_align() - Prepare refilling a page_frag with aligning
 * requirement.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the fragment
 *
 * Prepare refill a page_frag from page_frag cache with aligning requirement.
 *
 * Return:
 * True if prepare refilling succeeds, otherwise return false.
 */
static inline bool __page_frag_refill_prepare_align(struct page_frag_cache *nc,
						    unsigned int fragsz,
						    struct page_frag *pfrag,
						    gfp_t gfp_mask,
						    unsigned int align_mask)
{
	return !!__page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask, align_mask);
}

/**
 * page_frag_refill_prepare_align() - Prepare refilling a page_frag with aligning
 * requirement.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache needs to be refilled
 * @align: the requested aligning requirement for the fragment
 *
 * WARN_ON_ONCE() checking for @align before prepare refilling a page_frag from
 * page_frag cache with aligning requirement.
 *
 * Return:
 * True if prepare refilling succeeds, otherwise return false.
 */
static inline bool page_frag_refill_prepare_align(struct page_frag_cache *nc,
						  unsigned int fragsz,
						  struct page_frag *pfrag,
						  gfp_t gfp_mask,
						  unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_refill_prepare_align(nc, fragsz, pfrag, gfp_mask, -align);
}

/**
 * page_frag_refill_prepare() - Prepare refilling a page_frag.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Prepare refilling a page_frag from page_frag cache.
 *
 * Return:
 * True if refill succeeds, otherwise return false.
 */
static inline bool page_frag_refill_prepare(struct page_frag_cache *nc,
					    unsigned int fragsz,
					    struct page_frag *pfrag,
					    gfp_t gfp_mask)
{
	return __page_frag_refill_prepare_align(nc, fragsz, pfrag, gfp_mask, ~0u);
}

/**
 * __page_frag_alloc_refill_prepare_align() - Prepare allocing a fragment and
 * refilling a page_frag with aligning requirement.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align_mask: the requested aligning requirement for the fragment.
 *
 * Prepare allocing a fragment and refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *__page_frag_alloc_refill_prepare_align(struct page_frag_cache *nc,
							   unsigned int fragsz,
							   struct page_frag *pfrag,
							   gfp_t gfp_mask,
							   unsigned int align_mask)
{
	return __page_frag_cache_prepare(nc, fragsz, pfrag, gfp_mask, align_mask);
}

/**
 * page_frag_alloc_refill_prepare_align() - Prepare allocing a fragment and
 * refilling a page_frag with aligning requirement.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 * @align: the requested aligning requirement for the fragment.
 *
 * WARN_ON_ONCE() checking for @align before prepare allocing a fragment and
 * refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_refill_prepare_align(struct page_frag_cache *nc,
							 unsigned int fragsz,
							 struct page_frag *pfrag,
							 gfp_t gfp_mask,
							 unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_refill_prepare_align(nc, fragsz, pfrag, gfp_mask, -align);
}

/**
 * page_frag_alloc_refill_prepare() - Prepare allocing a fragment and refilling
 * a page_frag.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @gfp_mask: the allocation gfp to use when cache need to be refilled
 *
 * Prepare allocing a fragment and refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_refill_prepare(struct page_frag_cache *nc,
						   unsigned int fragsz,
						   struct page_frag *pfrag,
						   gfp_t gfp_mask)
{
	return __page_frag_alloc_refill_prepare_align(nc, fragsz, pfrag, gfp_mask, ~0u);
}

/**
 * __page_frag_alloc_refill_probe_align() - Probe allocing a fragment and refilling
 * a page_frag with aligning requirement.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled.
 * @align_mask: the requested aligning requirement for the fragment.
 *
 * Probe allocing a fragment and refilling a page_frag from page_frag cache with
 * aligning requirement.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *__page_frag_alloc_refill_probe_align(struct page_frag_cache *nc,
							 unsigned int fragsz,
							 struct page_frag *pfrag,
							 unsigned int align_mask)
{
	unsigned long encoded_page = nc->encoded_page;
	unsigned int size, offset;

	size = page_frag_cache_page_size(encoded_page);
	offset = __ALIGN_KERNEL_MASK(nc->offset, ~align_mask);
	if (unlikely(!encoded_page || offset + fragsz > size))
		return NULL;

	pfrag->page = page_frag_encoded_page_ptr(encoded_page);
	pfrag->size = size - offset;
	pfrag->offset = offset;

	return page_frag_encoded_page_address(encoded_page) + offset;
}

/**
 * page_frag_alloc_refill_probe() - Probe allocing a fragment and refilling
 * a page_frag.
 * @nc: page_frag cache from which to allocate and refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled
 *
 * Probe allocing a fragment and refilling a page_frag from page_frag cache.
 *
 * Return:
 * virtual address of the page fragment, otherwise return NULL.
 */
static inline void *page_frag_alloc_refill_probe(struct page_frag_cache *nc,
						 unsigned int fragsz,
						 struct page_frag *pfrag)
{
	return __page_frag_alloc_refill_probe_align(nc, fragsz, pfrag, ~0u);
}

/**
 * page_frag_refill_probe() - Probe refilling a page_frag.
 * @nc: page_frag cache from which to refill
 * @fragsz: the requested fragment size
 * @pfrag: the page_frag to be refilled
 *
 * Probe refilling a page_frag from page_frag cache.
 *
 * Return:
 * True if refill succeeds, otherwise return false.
 */
static inline bool page_frag_refill_probe(struct page_frag_cache *nc,
					  unsigned int fragsz,
					  struct page_frag *pfrag)
{
	return !!page_frag_alloc_refill_probe(nc, fragsz, pfrag);
}

/**
 * page_frag_commit - Commit allocing a page fragment.
 * @nc: page_frag cache from which to commit
 * @pfrag: the page_frag to be committed
 * @used_sz: size of the page fragment has been used
 *
 * Commit the actual used size for the allocation that was either prepared
 * or probed.
 */
static inline void page_frag_commit(struct page_frag_cache *nc, struct page_frag *pfrag,
				    unsigned int used_sz)
{
	__page_frag_cache_commit(nc, pfrag, true, used_sz);
}

/**
 * page_frag_commit_noref - Commit allocing a page fragment without taking
 * page refcount.
 * @nc: page_frag cache from which to commit
 * @pfrag: the page_frag to be committed
 * @used_sz: size of the page fragment has been used
 *
 * Commit the alloc preparing or probing by passing the actual used size, but
 * not taking refcount. Mostly used for fragmemt coalescing case when the
 * current fragment can share the same refcount with previous fragment.
 */
static inline void page_frag_commit_noref(struct page_frag_cache *nc,
					  struct page_frag *pfrag, unsigned int used_sz)
{
	__page_frag_cache_commit(nc, pfrag, false, used_sz);
}

/**
 * page_frag_alloc_abort - Abort the page fragment allocation.
 * @nc: page_frag cache to which the page fragment is aborted back
 * @fragsz: size of the page fragment to be aborted
 *
 * It is expected to be called from the same context as the alloc API.
 * Mostly used for error handling cases where the fragment is no longer needed.
 */
static inline void page_frag_alloc_abort(struct page_frag_cache *nc, unsigned int fragsz)
{
	VM_BUG_ON(fragsz > nc->offset);

	nc->pagecnt_bias++;
	nc->offset -= fragsz;
}

void page_frag_free(void *addr);

#endif
