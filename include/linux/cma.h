/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CMA_H__
#define __CMA_H__

#include <linux/init.h>
#include <linux/types.h>
#include <linux/numa.h>

#ifdef CONFIG_CMA_AREAS
#define MAX_CMA_AREAS	CONFIG_CMA_AREAS
#endif

#define CMA_MAX_NAME 64

/*
 *  the buddy -- especially pageblock merging and alloc_contig_range()
 * -- can deal with only some pageblocks of a higher-order page being
 *  MIGRATE_CMA, we can use pageblock_nr_pages.
 */
#define CMA_MIN_ALIGNMENT_PAGES pageblock_nr_pages
#define CMA_MIN_ALIGNMENT_BYTES (PAGE_SIZE * CMA_MIN_ALIGNMENT_PAGES)

enum cma_flags {
	__CMA_RESERVE_PAGES_ON_ERROR,
	__CMA_ZONES_VALID,
	__CMA_ZONES_INVALID,
	__CMA_ACTIVATED,
	__CMA_FIXED,
};

#define CMA_RESERVE_PAGES_ON_ERROR	BIT(__CMA_RESERVE_PAGES_ON_ERROR)
#define CMA_ZONES_VALID			BIT(__CMA_ZONES_VALID)
#define CMA_ZONES_INVALID		BIT(__CMA_ZONES_INVALID)
#define CMA_ACTIVATED			BIT(__CMA_ACTIVATED)
#define CMA_FIXED			BIT(__CMA_FIXED)

#define CMA_INIT_FLAGS (CMA_FIXED|CMA_RESERVE_PAGES_ON_ERROR)

struct cma;
struct zone;

extern unsigned long totalcma_pages;
extern phys_addr_t cma_get_base(const struct cma *cma);
extern unsigned long cma_get_size(const struct cma *cma);
extern const char *cma_get_name(const struct cma *cma);

extern int __init cma_declare_contiguous_nid(phys_addr_t base,
			phys_addr_t size, phys_addr_t limit,
			phys_addr_t alignment, unsigned int order_per_bit,
			unsigned long flags, const char *name,
			struct cma **res_cma, int nid);
static inline int __init cma_declare_contiguous(phys_addr_t base,
			phys_addr_t size, phys_addr_t limit,
			phys_addr_t alignment, unsigned int order_per_bit,
			unsigned long flags, const char *name,
			struct cma **res_cma)
{
	return cma_declare_contiguous_nid(base, size, limit, alignment,
			order_per_bit, flags, name, res_cma, NUMA_NO_NODE);
}
extern int __init cma_declare_contiguous_multi(phys_addr_t size,
			phys_addr_t align, unsigned int order_per_bit,
			unsigned long flags, const char *name,
			struct cma **res_cma, int nid);
extern int cma_init_reserved_mem(phys_addr_t base, phys_addr_t size,
					unsigned int order_per_bit,
					unsigned long flags,
					const char *name, struct cma **res_cma);
extern struct page *cma_alloc(struct cma *cma, unsigned long count, unsigned int align,
			      bool no_warn);
extern bool cma_pages_valid(struct cma *cma, const struct page *pages, unsigned long count);
extern bool cma_release(struct cma *cma, const struct page *pages, unsigned long count);

extern int cma_for_each_area(int (*it)(struct cma *cma, void *data), void *data);
extern bool cma_intersects(struct cma *cma, unsigned long start, unsigned long end);

extern void cma_reserve_pages_on_error(struct cma *cma);

#ifdef CONFIG_CMA
struct folio *cma_alloc_folio(struct cma *cma, int order, gfp_t gfp);
bool cma_free_folio(struct cma *cma, const struct folio *folio);
bool cma_validate_zones(struct cma *cma);
int cma_numranges(void);
unsigned long cma_get_available(const struct cma *cma);
bool cma_next_balance_pagerange(struct zone *zone, struct cma *cma, int *rindex,
			    unsigned long *startpfn, unsigned long *endpfn);
bool cma_next_noncma_pagerange(struct zone *zone, int *rindex,
			       unsigned long *startpfn, unsigned long *endpfn);
#else
static inline struct folio *cma_alloc_folio(struct cma *cma, int order, gfp_t gfp)
{
	return NULL;
}

static inline bool cma_free_folio(struct cma *cma, const struct folio *folio)
{
	return false;
}
static inline bool cma_validate_zones(struct cma *cma)
{
	return false;
}

static inline int cma_numranges(void)
{
	return 0;
}

static inline unsigned long cma_get_available(const struct cma *cma)
{
	return 0;
}

static inline bool cma_next_balance_pagerange(struct zone *zone,
			struct cma *cma, int *rindex, unsigned long *start_pfn,
			unsigned long *end_pfn)
{
	return false;
}

static inline bool cma_next_noncma_pagerange(struct zone *zone, int *rindex,
			     unsigned long *start_pfn, unsigned long *end_pfn)
{
	return false;
}
#endif

#endif
