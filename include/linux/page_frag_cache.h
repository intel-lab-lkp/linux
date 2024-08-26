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

static inline void page_frag_cache_init(struct page_frag_cache *nc)
{
	nc->encoded_page = 0;
}

static inline bool page_frag_cache_is_pfmemalloc(struct page_frag_cache *nc)
{
	return page_frag_encoded_page_pfmemalloc(nc->encoded_page);
}

static inline unsigned int page_frag_cache_page_size(unsigned long encoded_page)
{
	return PAGE_SIZE << page_frag_encoded_page_order(encoded_page);
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

static inline void *page_frag_alloc_align(struct page_frag_cache *nc,
					  unsigned int fragsz, gfp_t gfp_mask,
					  unsigned int align)
{
	WARN_ON_ONCE(!is_power_of_2(align));
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, -align);
}

static inline void *page_frag_alloc(struct page_frag_cache *nc,
				    unsigned int fragsz, gfp_t gfp_mask)
{
	return __page_frag_alloc_align(nc, fragsz, gfp_mask, ~0u);
}

void page_frag_free(void *addr);

#endif
