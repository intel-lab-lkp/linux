// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * DMA_PMD sparse per-PMD-frame metadata table.
 *
 * See Documentation/core-api/dma-pmd.rst for the architecture overview.
 */

#include <linux/bitmap.h>
#include <linux/cache.h>
#include <linux/cacheflush.h>
#include <linux/dma-pmd.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include "dma-pmd-priv.h"

/*
 * Each PAGE_SIZE (4KB) metadata page holds (PAGE_SIZE >> DMA_PMD_META_SHIFT)
 * struct dma_pmd_meta entries (32 entries of 128B, or 16 entries of 256B with
 * spinlock debugging), each covering one PMD_SIZE (2MB) physical frame.
 * One metadata page therefore covers a chunk of DMA_PMD_CHUNK_PAGES 4KB pages
 * (64MB normally, or 32MB with spinlock debugging), i.e. order
 * DMA_PMD_CHUNK_ORDER.
 */
#define DMA_PMD_CHUNK_ORDER		(PMD_ORDER + PAGE_SHIFT - DMA_PMD_META_SHIFT)
#define DMA_PMD_CHUNK_PAGES		BIT(DMA_PMD_CHUNK_ORDER)

static_assert(PAGE_SIZE >= DMA_PMD_META_SIZE);
static_assert(PMD_SIZE == SZ_2M);
static_assert(PMD_ORDER <= MAX_PAGE_ORDER);
static_assert(PMD_SHIFT <= SUBSECTION_SHIFT);

void *dma_pmd_meta_array __read_mostly;
EXPORT_SYMBOL(dma_pmd_meta_array);
unsigned long dma_pmd_meta_nframes __read_mostly;
static unsigned long *dma_pmd_chunk_bitmap __read_mostly;

static struct dma_pmd_meta dma_pmd_meta_nil;

static unsigned long dma_pmd_meta_pages;
static DEFINE_MUTEX(dma_pmd_meta_mutex);

static __always_inline struct dma_pmd_meta *__dma_pmd_meta_of_pfn(unsigned long pfn)
{
	struct dma_pmd_meta *array = dma_pmd_meta_base();
	unsigned long frame = pfn >> PMD_ORDER;

	if (unlikely(!array || frame >= dma_pmd_meta_nframes ||
		     !test_bit_acquire(pfn >> DMA_PMD_CHUNK_ORDER, dma_pmd_chunk_bitmap)))
		return &dma_pmd_meta_nil;

	return &array[frame];
}

bool __dma_is_pmd_page(unsigned long pfn)
{
	return READ_ONCE(__dma_pmd_meta_of_pfn(pfn)->pooled);
}
EXPORT_SYMBOL(__dma_is_pmd_page);

/**
 * dma_pmd_meta_of_pfn - Metadata for a PFN known to be used by DMA_PMD.
 * @pfn: PFN the caller already holds a struct page for
 */
struct dma_pmd_meta *dma_pmd_meta_of_pfn(unsigned long pfn)
{
	return __dma_pmd_meta_of_pfn(pfn);
}
EXPORT_SYMBOL(dma_pmd_meta_of_pfn);

/**
 * dma_pmd_meta_from_phys - Metadata for the PMD frame containing @pa
 * @pa: Any physical address
 *
 * Safe against MMIO above max_pfn and PFNs inside physical holes: those
 * resolve to the shared sink entry.
 *
 * Return: Pointer to metadata entry (never NULL).
 */
struct dma_pmd_meta *dma_pmd_meta_from_phys(phys_addr_t pa)
{
	return __dma_pmd_meta_of_pfn(PHYS_PFN(pa));
}
EXPORT_SYMBOL(dma_pmd_meta_from_phys);

/**
 * dma_pmd_meta_to_pfn - Base PFN of @m's PMD frame
 * @m: Entry in @dma_pmd_meta_array
 *
 * Return: PMD-aligned base PFN, or ULONG_MAX for the sink.
 */
unsigned long dma_pmd_meta_to_pfn(const struct dma_pmd_meta *m)
{
	if (unlikely(m == &dma_pmd_meta_nil))
		return ULONG_MAX;

	return (unsigned long)(m - dma_pmd_meta_base()) << PMD_ORDER;
}
EXPORT_SYMBOL(dma_pmd_meta_to_pfn);

/**
 * dma_pmd_meta_to_phys - Base physical address of @m's PMD frame
 * @m: Entry returned by dma_pmd_meta_of_pfn() or dma_pmd_meta_from_phys()
 *
 * Return: PMD-aligned physical address, or PHYS_ADDR_MAX for the sink.
 */
phys_addr_t dma_pmd_meta_to_phys(const struct dma_pmd_meta *m)
{
	if (unlikely(m == &dma_pmd_meta_nil))
		return PHYS_ADDR_MAX;

	return (phys_addr_t)(m - dma_pmd_meta_base()) << PMD_SHIFT;
}
EXPORT_SYMBOL(dma_pmd_meta_to_phys);

/*
 * Install one preallocated page. The page is allocated by the caller rather
 * than here because apply_to_page_range() runs this callback under lazy-MMU
 * mode with the pte level pinned, which is not a context to allocate from.
 *
 * @data points at the caller's page pointer and is cleared once the page has
 * been consumed, so the caller can free it if it was not needed.
 */
static int dma_pmd_meta_set_pte(pte_t *ptep, unsigned long addr, void *data)
{
	struct page **pagep = data;
	pte_t pte;

	if (!pte_none(ptep_get(ptep)))
		return 0;

	pte = pfn_pte(page_to_pfn(*pagep), PAGE_KERNEL);

	spin_lock(&init_mm.page_table_lock);
	if (likely(pte_none(ptep_get(ptep)))) {
		set_pte_at(&init_mm, addr, ptep, pte);
		*pagep = NULL;
	}
	spin_unlock(&init_mm.page_table_lock);

	return 0;
}

/* True iff @pfn is backed by online system RAM (excludes holes and ZONE_DEVICE). */
static inline bool dma_pmd_pfn_online(unsigned long pfn)
{
	struct mem_section *ms;

	if (unlikely(!pfn_valid(pfn)))
		return false;

	ms = __pfn_to_section(pfn);
	if (unlikely(!online_section(ms)))
		return false;

	if (unlikely(online_device_section(ms) && is_zone_device_page(pfn_to_page(pfn))))
		return false;

	return true;
}

/**
 * dma_pmd_meta_populate - back the array for [@start_pfn, @end_pfn)
 * @array: base virtual address of the reservation
 * @nframes: number of valid PMD frames in the reservation
 * @start_pfn: first PFN of the range
 * @end_pfn: one past the last PFN of the range
 * @force: if true, populate every chunk unconditionally (hotplug / arena)
 *
 * Maps and zeroes one page of the array per chunk of the range that contains
 * at least one online RAM PFN (or unconditionally when @force is set), and
 * skips chunks that are already mapped. A chunk
 * is DMA_PMD_CHUNK_PAGES of physical address space: 64MB normally, 32MB when
 * spinlock debugging doubles the entry size.
 * Locking: caller must hold @dma_pmd_meta_mutex.
 *
 * Return: 0, or -ENOMEM with the range partially backed.
 */
static int dma_pmd_meta_populate(struct dma_pmd_meta *array, unsigned long nframes,
				 unsigned long start_pfn, unsigned long end_pfn, bool force)
{
	unsigned long chunk, last, pfn;

	lockdep_assert_held(&dma_pmd_meta_mutex);

	end_pfn = min(end_pfn, nframes << PMD_ORDER);
	if (start_pfn >= end_pfn)
		return 0;

	chunk = start_pfn >> DMA_PMD_CHUNK_ORDER;
	last = (end_pfn - 1) >> DMA_PMD_CHUNK_ORDER;

	for (; chunk <= last; chunk++) {
		unsigned long addr, base = chunk << DMA_PMD_CHUNK_ORDER;
		int ret, nid = NUMA_NO_NODE;
		bool has_valid = false;
		struct page *page;

		addr = (unsigned long)array + (chunk << PAGE_SHIFT);
		if (vmalloc_to_page((void *)addr)) {
			set_bit(chunk, dma_pmd_chunk_bitmap);
			continue;		/* already backed */
		}

		if (force) {
			has_valid = true;
		} else {
			for (pfn = base; pfn < base + DMA_PMD_CHUNK_PAGES;
			     pfn += 1UL << PMD_ORDER) {
				if (dma_pmd_pfn_online(pfn)) {
					has_valid = true;
					nid = page_to_nid(pfn_to_page(pfn));
					if (nid != NUMA_NO_NODE && node_online(nid))
						break;
					nid = NUMA_NO_NODE;
				}
			}
		}
		if (!has_valid)
			continue;	/* pure hole, leave it unmapped */

		page = alloc_pages_node(nid, GFP_KERNEL | __GFP_ZERO, 0);
		if (!page)
			return -ENOMEM;

		ret = apply_to_page_range(&init_mm, addr, PAGE_SIZE,
					  dma_pmd_meta_set_pte, &page);
		if (ret) {
			__free_page(page);
			return ret;
		}
		if (page) {
			/*
			 * Unreachable while dma_pmd_meta_mutex serialises
			 * every install, but kept so that the defensive
			 * pte_none() re-test in dma_pmd_meta_set_pte() can
			 * never leak the page it declined to consume.
			 */
			__free_page(page);
			set_bit(chunk, dma_pmd_chunk_bitmap);
			continue;
		}

		flush_cache_vmap(addr, addr + PAGE_SIZE);
		/* Pair with test_bit_acquire() in readers. */
		smp_mb__before_atomic();
		set_bit(chunk, dma_pmd_chunk_bitmap);
		dma_pmd_meta_pages++;
	}

	return 0;
}

bool dma_pmd_meta_ensure_pfn(unsigned long pfn, bool can_block)
{
	int ret;

	if (likely(__dma_pmd_meta_of_pfn(pfn) != &dma_pmd_meta_nil))
		return true;

	if (!can_block || !dma_pmd_meta_base() ||
	    (pfn >> PMD_ORDER) >= dma_pmd_meta_nframes)
		return false;

	mutex_lock(&dma_pmd_meta_mutex);
	ret = dma_pmd_meta_populate(dma_pmd_meta_base(), dma_pmd_meta_nframes,
				    pfn, pfn + (1UL << PMD_ORDER), true);
	mutex_unlock(&dma_pmd_meta_mutex);

	return !ret;
}
EXPORT_SYMBOL(dma_pmd_meta_ensure_pfn);

/**
 * dma_pmd_meta_init - Reserve and populate the sparse per-PMD metadata array
 *
 * Populates all present RAM pages before publishing @dma_pmd_meta_array so
 * no concurrent reader ever sees an unmapped entry.
 *
 * Return: 0 on success, or negative errno on failure.
 */
int dma_pmd_meta_init(void)
{
	unsigned long nframes, nchunks, size;
	struct dma_pmd_meta *array;
	struct vm_struct *vm;
	int ret = 0;

	if (likely(dma_pmd_meta_base()))
		return 0;

	mutex_lock(&dma_pmd_meta_mutex);
	if (dma_pmd_meta_array)
		goto out_unlock;

	nframes = DIV_ROUND_UP(max3((unsigned long)max_pfn,
				    (unsigned long)max_possible_pfn,
				    (unsigned long)min_t(u64, iomem_resource.end >> PAGE_SHIFT,
							 1ULL << (MAX_PHYSMEM_BITS - PAGE_SHIFT))),
			       1UL << PMD_ORDER);
	size = PAGE_ALIGN(nframes << DMA_PMD_META_SHIFT);
	nchunks = size >> PAGE_SHIFT;

	dma_pmd_chunk_bitmap = bitmap_zalloc(nchunks, GFP_KERNEL);
	if (!dma_pmd_chunk_bitmap) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	vm = get_vm_area(size, VM_MAP);
	if (!vm) {
		bitmap_free(dma_pmd_chunk_bitmap);
		dma_pmd_chunk_bitmap = NULL;
		ret = -ENOMEM;
		goto out_unlock;
	}
	array = vm->addr;

	ret = dma_pmd_meta_populate(array, nframes, 0, max_pfn, false);
	if (ret) {
		struct page *p, *next;
		unsigned long chunk;
		LIST_HEAD(pages);

		for_each_set_bit(chunk, dma_pmd_chunk_bitmap, nchunks) {
			p = vmalloc_to_page((void *)array + (chunk << PAGE_SHIFT));
			if (p)
				list_add(&p->lru, &pages);
		}
		dma_pmd_meta_pages = 0;
		free_vm_area(vm);
		bitmap_free(dma_pmd_chunk_bitmap);
		dma_pmd_chunk_bitmap = NULL;
		list_for_each_entry_safe(p, next, &pages, lru) {
			list_del_init(&p->lru);
			__free_page(p);
		}
		goto out_unlock;
	}

	/*
	 * Publish @nframes and the populated pages before the array pointer:
	 * readers pair with smp_load_acquire(&dma_pmd_meta_array).
	 */
	dma_pmd_meta_nframes = nframes;
	/* Pairs with smp_load_acquire() in dma_is_pmd_page(). */
	smp_store_release(&dma_pmd_meta_array, array);

	pr_info("dma_pmd: %lu frames, %lu KB of KVA, %lu pages backed (%lu KB)\n",
		nframes, size / 1024, dma_pmd_meta_pages,
		dma_pmd_meta_pages * (PAGE_SIZE / 1024));

out_unlock:
	mutex_unlock(&dma_pmd_meta_mutex);
	return ret;
}
EXPORT_SYMBOL(dma_pmd_meta_init);
