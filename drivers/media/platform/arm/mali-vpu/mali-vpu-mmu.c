// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/build_bug.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/slab.h>

#include "mali-vpu-mmu.h"

/* Number of bits for the VPU physical (DMA) address space. */
#define MALI_VPU_PA_BITS		40
#define MALI_VPU_PA_MASK		GENMASK_ULL(MALI_VPU_PA_BITS - 1, 0)

/* Number of bits for the virtual address space. */
#define MALI_VPU_VA_BITS		32
#define MALI_VPU_VA_MASK		GENMASK(MALI_VPU_VA_BITS - 1, 0)

/* Number of bits from the VA used to index a PTE in a page. */
#define MALI_VPU_INDEX_SHIFT		10
#define MALI_VPU_INDEX_SIZE		BIT(MALI_VPU_INDEX_SHIFT)
#define MALI_VPU_INDEX_MASK		GENMASK(MALI_VPU_INDEX_SHIFT - 1, 0)

/* Access permission defines. */
#define MALI_VPU_PTE_AP_SHIFT		0
#define MALI_VPU_PTE_AP_BITS		2
#define MALI_VPU_PTE_AP_MASK		((1 << MALI_VPU_PTE_AP_BITS) - 1)

/* PTE shift defines. */
#define MALI_VPU_PTE_PHYSADDR_SHIFT	2
#define MALI_VPU_PTE_ATTR_SHIFT		30

/* Number of levels for Page Table Walk. */
#define MALI_VPU_PTW_LEVELS		2

/*
 * A Linux physical page can be equal in size or larger than the MVE page size.
 * This define calculates how many MVE pages fit in one Linux page.
 */
#define MALI_VPU_PAGES_PER_LINUX_PAGE	(PAGE_SIZE / MALI_VPU_PAGE_SIZE)

/*
 * If the Linux PAGE_SIZE were ever smaller than MALI_VPU_PAGE_SIZE, the define
 * above would evaluate to 0 and break the page accounting. This static_assert enforces
 * this invariant at build time.
 */
static_assert(MALI_VPU_PAGES_PER_LINUX_PAGE > 0,
	      "PAGE_SIZE must be >= MALI_VPU_PAGE_SIZE");

/*
 * Return the PTE index for a given level (L1=0, L2=1).
 *                   22                  12                       0
 * +-------------------+-------------------+-----------------------+
 * |      Level 1      |      Level 2      |      Page offset      |
 * +-------------------+-------------------+-----------------------+
 */
static unsigned int mali_vpu_get_index(const mali_vpu_mmu_va va, const unsigned int level)
{
	return (va >> (MALI_VPU_PAGE_SHIFT + (MALI_VPU_PTW_LEVELS - level - 1) *
			MALI_VPU_INDEX_SHIFT)) & MALI_VPU_INDEX_MASK;
}

static mali_vpu_mmu_pte mali_vpu_mmu_set_pte(enum mali_vpu_mmu_attr attr,
					     dma_addr_t dma,
					     enum mali_vpu_mmu_access access)
{
	return (attr << MALI_VPU_PTE_ATTR_SHIFT) |
		((dma >> MALI_VPU_PAGE_SHIFT) << MALI_VPU_PTE_PHYSADDR_SHIFT) |
		(access << MALI_VPU_PTE_AP_SHIFT);
}

static mali_vpu_mmu_pte *mali_vpu_page_table_walk(struct mali_vpu_mmu *mmu, mali_vpu_mmu_va va,
						  bool alloc)
{
	struct mali_vpu_mmu_pt_page *l2;
	mali_vpu_mmu_pte *pte = mmu->root.cpu;
	unsigned int index;

	/* Level 1. */
	index = mali_vpu_get_index(va, 0);
	l2 = &mmu->l2_tables[index];

	/* Map in L2 page if it is missing. */
	if (!l2->linux_page) {
		if (!alloc) {
			dev_err(mmu->dev, "missing L2 page in PTW. va=0x%x", va);
			return ERR_PTR(-EFAULT);
		}

		l2->dma = mali_vpu_mmu_alloc_page(mmu->dev, &l2->linux_page, DMA_TO_DEVICE);
		if (!l2->linux_page) {
			dev_err(mmu->dev, "failed to allocate L2 page. va=0x%x", va);
			return ERR_PTR(-ENOMEM);
		}

		l2->cpu = page_address(l2->linux_page);

		pte[index] = mali_vpu_mmu_set_pte(MALI_VPU_ATTR_PRIVATE, l2->dma,
						  MALI_VPU_ACCESS_READ_ONLY);
		dma_sync_single_for_device(mmu->dev, mmu->root.dma + index * sizeof(*pte),
					   sizeof(*pte), DMA_TO_DEVICE);
	}

	/* Level 2. */
	index = mali_vpu_get_index(va, 1);

	/* The caller must sync the entry for the device after modifying it. */
	return &l2->cpu[index];
}

static int mali_vpu_map_page(struct mali_vpu_mmu *mmu,
			     mali_vpu_mmu_va va,
			     dma_addr_t dma,
			     enum mali_vpu_mmu_attr attr,
			     enum mali_vpu_mmu_access access)
{
	struct mali_vpu_mmu_pt_page *l2;
	mali_vpu_mmu_pte *pte;

	/* Check that both VA and DMA address are page aligned. */
	if ((va | dma) & MALI_VPU_PAGE_MASK) {
		dev_err(mmu->dev, "VA and DMA address must be page aligned. va=0x%x, dma=%pad",
			va, &dma);
		return -EFAULT;
	}

	/* Check that VA is within valid address range. */
	if (va & ~MALI_VPU_VA_MASK) {
		dev_err(mmu->dev, "VA out of valid range. va=0x%x", va);
		return -EFAULT;
	}

	/* Check that the DMA address is within valid address range. */
	if (dma & ~MALI_VPU_PA_MASK) {
		dev_err(mmu->dev, "DMA address out of valid range. dma=%pad", &dma);
		return -EFAULT;
	}

	pte = mali_vpu_page_table_walk(mmu, va, true);
	if (IS_ERR(pte))
		return PTR_ERR(pte);

	/* Return error if the entry is already mapped or reserved. */
	if (*pte)
		return -EAGAIN;

	/* Map in DMA address and flush data. */
	*pte = mali_vpu_mmu_set_pte(attr, dma, access);
	l2 = &mmu->l2_tables[mali_vpu_get_index(va, 0)];
	dma_sync_single_for_device(mmu->dev,
				   l2->dma + mali_vpu_get_index(va, 1) * sizeof(*pte),
				   sizeof(*pte), DMA_TO_DEVICE);

	return 0;
}

static void mali_vpu_unmap_page(struct mali_vpu_mmu *mmu, mali_vpu_mmu_va va)
{
	struct mali_vpu_mmu_pt_page *l2;
	mali_vpu_mmu_pte *pte;

	pte = mali_vpu_page_table_walk(mmu, va, false);
	if (IS_ERR(pte))
		return;

	/* Unmap virtual address and flush data. */
	*pte = 0;
	l2 = &mmu->l2_tables[mali_vpu_get_index(va, 0)];
	dma_sync_single_for_device(mmu->dev,
				   l2->dma + mali_vpu_get_index(va, 1) * sizeof(*pte),
				   sizeof(*pte), DMA_TO_DEVICE);
}

static int mali_vpu_remap_page(struct mali_vpu_mmu *mmu,
			       mali_vpu_mmu_va va,
			       dma_addr_t dma,
			       enum mali_vpu_mmu_attr attr,
			       enum mali_vpu_mmu_access access)
{
	mali_vpu_unmap_page(mmu, va);
	return mali_vpu_map_page(mmu, va, dma, attr, access);
}

static int mali_vpu_remap_pages(struct mali_vpu_mmu_pages *pages, size_t oldcount,
				size_t newcount)
{
	int ret;

	if (!pages->mmu)
		return 0;

	/* Remap pages to no access if new count is smaller than old count. */
	while (newcount < oldcount) {
		oldcount--;

		ret = mali_vpu_remap_page(pages->mmu,
					  pages->va + oldcount * MALI_VPU_PAGE_SIZE,
					  MALI_VPU_PAGE_SIZE, MALI_VPU_ATTR_PRIVATE,
					  MALI_VPU_ACCESS_NO);
		if (ret)
			return ret;
	}

	/* Map up pages if new count is larger than old count. */
	while (newcount > oldcount) {
		ret = mali_vpu_remap_page(pages->mmu,
					  pages->va + oldcount * MALI_VPU_PAGE_SIZE,
					  pages->pages[oldcount].dma, pages->attr,
					  pages->access);
		if (ret)
			return ret;

		oldcount++;
	}

	return 0;
}

static int mali_vpu_mapped_count(const mali_vpu_mmu_pte *l2)
{
	int count = 0;
	unsigned int j;

	for (j = 0; j < MALI_VPU_INDEX_SIZE; j++) {
		/* Do not count pages with AP as MALI_VPU_ACCESS_NO */
		if (l2[j] & MALI_VPU_PTE_AP_MASK)
			count++;
	}

	return count;
}

static size_t mali_vpu_get_sg_table_npages(struct sg_table *sgt)
{
	struct sg_dma_page_iter piter;
	size_t count = 0;

	for_each_sg_dma_page(sgt->sgl, &piter, sgt->nents, 0) {
		count++;
	}

	return count;
}

static int mali_vpu_append_sg_table(struct mali_vpu_mmu_pages *pages, struct sg_table *sgt)
{
	size_t count;
	struct sg_dma_page_iter piter;

	/*
	 * SG import must describe complete Linux pages. VB2 DMA-SG MMAP buffers
	 * are allocated as whole pages with zero offset. DMA-BUF attachment
	 * mappings guarantee PAGE_SIZE-aligned DMA addresses and lengths.
	 * USERPTR is disabled on both queues because it can supply partial pages.
	 *
	 * Each Linux page is split into MALI_VPU_PAGES_PER_LINUX_PAGE VPU pages.
	 * Do not round DMA addresses down: the supplied mapping must already
	 * cover every page inserted into the VPU page tables.
	 */

	count = mali_vpu_get_sg_table_npages(sgt) * MALI_VPU_PAGES_PER_LINUX_PAGE;

	if ((pages->count + count) > pages->capacity) {
		dev_err(pages->dev,
			"pages capacity too small to append sgt. count=%zu, capacity=%zu, append=%zu",
			pages->count, pages->capacity, count);
		return -ENOMEM;
	}

	for_each_sg_dma_page(sgt->sgl, &piter, sgt->nents, 0) {
		unsigned int j;
		dma_addr_t base;

		base = sg_page_iter_dma_address(&piter);

		for (j = 0; j < MALI_VPU_PAGES_PER_LINUX_PAGE; ++j)
			pages->pages[pages->count++].dma =
				base + j * MALI_VPU_PAGE_SIZE;
	}

	return 0;
}

int mali_vpu_mmu_init(struct mali_vpu_mmu *mmu, struct device *dev)
{
	struct page *page;
	dma_addr_t dma;

	mmu->dev = dev;

	/* Allocate Page Table Base (the L1 table). */
	dma = mali_vpu_mmu_alloc_page(dev, &page, DMA_TO_DEVICE);
	if (!page)
		return -ENOMEM;

	mmu->l2_tables = kvcalloc(MALI_VPU_INDEX_SIZE, sizeof(*mmu->l2_tables), GFP_KERNEL);
	if (!mmu->l2_tables) {
		mali_vpu_mmu_free_page(dev, dma, page, DMA_TO_DEVICE);
		return -ENOMEM;
	}

	mmu->root.linux_page = page;
	mmu->root.cpu = page_address(page);
	mmu->root.dma = dma;

	return 0;
}

void mali_vpu_mmu_cleanup(struct mali_vpu_mmu *mmu)
{
	int count = 0;
	unsigned int i;

	for (i = 0; i < MALI_VPU_INDEX_SIZE; i++) {
		struct mali_vpu_mmu_pt_page *l2 = &mmu->l2_tables[i];

		if (!l2->linux_page)
			continue;

		count += mali_vpu_mapped_count(l2->cpu);
		mali_vpu_mmu_free_page(mmu->dev, l2->dma, l2->linux_page, DMA_TO_DEVICE);
	}

	kvfree(mmu->l2_tables);
	mali_vpu_mmu_free_page(mmu->dev, mmu->root.dma, mmu->root.linux_page,
			       DMA_TO_DEVICE);

	WARN_ON_ONCE(count > 0);
}

/* Optionally return the original page for callers that track CPU access. */
dma_addr_t mali_vpu_mmu_alloc_page(struct device *dev, struct page **out_page,
				   enum dma_data_direction dir)
{
	struct page *page;
	dma_addr_t dma_handle;

	if (out_page)
		*out_page = NULL;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_NORETRY);
	if (!page)
		return 0;

	dma_handle = dma_map_page(dev, page, 0, PAGE_SIZE, dir);
	if (dma_mapping_error(dev, dma_handle) != 0) {
		dev_err(dev, "cannot map page to DMA address space. page=%p", page);
		goto free_page;
	}

	dma_sync_single_for_device(dev, dma_handle, PAGE_SIZE, dir);

	if (out_page)
		*out_page = page;

	return dma_handle;

free_page:
	__free_page(page);
	return 0;
}

void mali_vpu_mmu_free_page(struct device *dev, dma_addr_t dma, struct page *page,
			    enum dma_data_direction dir)
{
	if (!page)
		return;

	dma_unmap_page(dev, dma, PAGE_SIZE, dir);
	__free_page(page);
}

struct mali_vpu_mmu_pages *mali_vpu_mmu_alloc_pages(struct device *dev,
						    size_t count,
						    size_t capacity)
{
	struct mali_vpu_mmu_pages *pages;
	int ret;

	count = roundup(count, MALI_VPU_PAGES_PER_LINUX_PAGE);
	capacity = roundup(capacity, MALI_VPU_PAGES_PER_LINUX_PAGE);
	capacity = max(count, capacity);

	pages = devm_kzalloc(dev,
			     sizeof(*pages) + sizeof(pages->pages[0]) * capacity,
			     GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	pages->dev = dev;
	pages->capacity = capacity;

	for (pages->count = 0; pages->count < count; ) {
		struct page *page;
		dma_addr_t dma;
		unsigned int i;

		/* Allocate one linux page containing one or more MVE pages. */
		dma = mali_vpu_mmu_alloc_page(dev, &page, DMA_BIDIRECTIONAL);
		if (!page) {
			ret = -ENOMEM;
			goto release_pages;
		}

		/*
		 * If the Linux page is larger than the MVE page, then
		 * we iterate and add DMA addresses with an offset from
		 * the Linux page.
		 */
		for (i = 0; i < MALI_VPU_PAGES_PER_LINUX_PAGE; i++) {
			pages->pages[pages->count].dma = dma + i * MALI_VPU_PAGE_SIZE;
			pages->pages[pages->count].linux_page = page;
			pages->count++;
		}
	}

	return pages;

release_pages:
	mali_vpu_mmu_free_pages(pages);

	return ERR_PTR(ret);
}

struct mali_vpu_mmu_pages *mali_vpu_mmu_alloc_pages_sg(struct device *dev,
						       struct sg_table *sgt,
						       size_t capacity)
{
	struct mali_vpu_mmu_pages *pages;
	size_t count;
	int ret;

	count = mali_vpu_get_sg_table_npages(sgt) * MALI_VPU_PAGES_PER_LINUX_PAGE;
	capacity = roundup(capacity, MALI_VPU_PAGES_PER_LINUX_PAGE);
	capacity = max(count, capacity);

	pages = devm_kzalloc(dev,
			     sizeof(*pages) + sizeof(pages->pages[0]) * capacity,
			     GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	pages->dev = dev;
	pages->capacity = capacity;
	pages->is_external = true;
	pages->offset = sgt->sgl ? sgt->sgl->offset : 0;

	ret = mali_vpu_append_sg_table(pages, sgt);
	if (ret) {
		devm_kfree(dev, pages);
		return ERR_PTR(ret);
	}

	return pages;
}

/* Release backing pages only; page-table updates are handled by the caller. */
static void mali_vpu_mmu_shrink_pages(struct mali_vpu_mmu_pages *pages, size_t npages)
{
	while (pages->count > npages) {
		pages->count--;

		if ((pages->count % MALI_VPU_PAGES_PER_LINUX_PAGE) == 0)
			mali_vpu_mmu_free_page(pages->dev,
					       pages->pages[pages->count].dma,
					       pages->pages[pages->count].linux_page,
					       DMA_BIDIRECTIONAL);

		pages->pages[pages->count].dma = 0;
		pages->pages[pages->count].linux_page = NULL;
	}
}

int mali_vpu_mmu_resize_pages(struct mali_vpu_mmu_pages *pages, size_t npages)
{
	int ret;
	size_t oldcount = pages->count;

	if (pages->is_external) {
		dev_err(pages->dev, "externally managed pages cannot be resized");
		return -EINVAL;
	}

	npages = roundup(npages, MALI_VPU_PAGES_PER_LINUX_PAGE);

	if (npages > pages->capacity) {
		dev_err(pages->dev,
			"new MMU pages size is larger than capacity. npages=%zu, capacity=%zu",
			npages, pages->capacity);
		return -ENOMEM;
	}

	/* Free pages if npage is smaller than allocated pages. */
	if (pages->count > npages) {
		/* Replace the PTEs before releasing their backing pages. */
		ret = mali_vpu_remap_pages(pages, oldcount, npages);
		if (ret)
			return ret;

		mali_vpu_mmu_shrink_pages(pages, npages);
		return 0;
	}

	/* Allocate pages if npage is larger than allocated pages. */
	while (pages->count < npages) {
		struct page *page;
		dma_addr_t dma;
		unsigned int i;

		dma = mali_vpu_mmu_alloc_page(pages->dev, &page, DMA_BIDIRECTIONAL);
		if (!page)
			goto rollback_alloc;

		for (i = 0; i < MALI_VPU_PAGES_PER_LINUX_PAGE; i++) {
			pages->pages[pages->count].dma = dma + i * MALI_VPU_PAGE_SIZE;
			pages->pages[pages->count].linux_page = page;
			pages->count++;
		}
	}

	return mali_vpu_remap_pages(pages, oldcount, pages->count);

rollback_alloc:
	mali_vpu_mmu_shrink_pages(pages, oldcount);
	return -ENOMEM;
}

void mali_vpu_mmu_free_pages(struct mali_vpu_mmu_pages *pages)
{
	unsigned int i;

	mali_vpu_mmu_unmap_pages(pages);

	if (!pages->is_external)
		for (i = 0; i < pages->count; i += MALI_VPU_PAGES_PER_LINUX_PAGE)
			mali_vpu_mmu_free_page(pages->dev, pages->pages[i].dma,
					       pages->pages[i].linux_page,
					       DMA_BIDIRECTIONAL);

	devm_kfree(pages->dev, pages);
}

size_t mali_vpu_mmu_size_pages(struct mali_vpu_mmu_pages *pages)
{
	return pages->count * MALI_VPU_PAGE_SIZE;
}

int mali_vpu_mmu_sync_pages(struct mali_vpu_mmu_pages *pages, enum dma_data_direction dir)
{
	size_t i;

	if (dir == DMA_FROM_DEVICE) {
		for (i = 0; i < pages->count; i += MALI_VPU_PAGES_PER_LINUX_PAGE)
			dma_sync_single_for_cpu(pages->dev, pages->pages[i].dma,
						PAGE_SIZE, DMA_FROM_DEVICE);
	} else if (dir == DMA_TO_DEVICE) {
		for (i = 0; i < pages->count; i += MALI_VPU_PAGES_PER_LINUX_PAGE)
			dma_sync_single_for_device(pages->dev, pages->pages[i].dma,
						   PAGE_SIZE, DMA_TO_DEVICE);
	} else {
		dev_err(pages->dev, "unsupported MMU flush direction. dir=%u", dir);
		return -EINVAL;
	}

	return 0;
}

int mali_vpu_mmu_map_pages(struct mali_vpu_mmu *mmu,
			   mali_vpu_mmu_va va,
			   struct mali_vpu_mmu_pages *pages,
			   enum mali_vpu_mmu_attr attr,
			   enum mali_vpu_mmu_access access)
{
	size_t i;
	int ret;

	/*
	 * A pages object may be mapped at multiple virtual addresses. For example,
	 * each core maps the same physical firmware text pages into its own virtual
	 * address region.
	 */
	for (i = 0; i < pages->count; i++) {
		ret = mali_vpu_map_page(mmu, va + i * MALI_VPU_PAGE_SIZE,
					pages->pages[i].dma, attr, access);
		if (ret)
			goto unmap_pages;
	}

	/*
	 * Reserve the rest of the address range. Adding a dummy page with
	 * DMA address 'MALI_VPU_PAGE_SIZE' should not lead to memory corruption,
	 * because the page is marked as 'no access'.
	 */
	for (; i < pages->capacity; i++) {
		ret = mali_vpu_map_page(mmu, va + i * MALI_VPU_PAGE_SIZE, MALI_VPU_PAGE_SIZE,
					MALI_VPU_ATTR_PRIVATE, MALI_VPU_ACCESS_NO);
		if (ret)
			goto unmap_pages;
	}

	pages->mmu = mmu;
	pages->va = va;
	pages->attr = attr;
	pages->access = access;

	return 0;

unmap_pages:
	while (i-- > 0)
		mali_vpu_unmap_page(mmu, va + i * MALI_VPU_PAGE_SIZE);

	return ret;
}

void mali_vpu_mmu_unmap_pages(struct mali_vpu_mmu_pages *pages)
{
	size_t i;

	if (!pages->mmu)
		return;

	for (i = 0; i < pages->capacity; i++)
		mali_vpu_unmap_page(pages->mmu, pages->va + i * MALI_VPU_PAGE_SIZE);

	pages->mmu = NULL;
	pages->va = 0;
}

int mali_vpu_mmu_map_dma(struct mali_vpu_mmu *mmu,
			 mali_vpu_mmu_va va,
			 dma_addr_t dma,
			 size_t size,
			 enum mali_vpu_mmu_attr attr,
			 enum mali_vpu_mmu_access access)
{
	int ret;
	size_t offset;

	for (offset = 0; offset < size; offset += MALI_VPU_PAGE_SIZE) {
		ret = mali_vpu_map_page(mmu, va + offset, dma + offset, attr, access);
		if (ret)
			goto unmap_pages;
	}

	return 0;

unmap_pages:
	/* Unroll mapped pages. */
	while (offset > 0) {
		offset -= MALI_VPU_PAGE_SIZE;
		mali_vpu_unmap_page(mmu, va + offset);
	}

	return ret;
}

void mali_vpu_mmu_unmap_va(struct mali_vpu_mmu *mmu,
			   mali_vpu_mmu_va va,
			   size_t size)
{
	size_t offset;

	for (offset = 0; offset < size; offset += MALI_VPU_PAGE_SIZE)
		mali_vpu_unmap_page(mmu, va + offset);
}

/* Initialize read-only firmware text before the VPU accesses these pages. */
int mali_vpu_mmu_write(struct mali_vpu_mmu_pages *pages,
		       const void *data,
		       size_t size)
{
	size_t i;

	if (pages->is_external || size > mali_vpu_mmu_size_pages(pages))
		return -EINVAL;

	for (i = 0; size; i += MALI_VPU_PAGES_PER_LINUX_PAGE) {
		dma_addr_t dma = pages->pages[i].dma;
		void *dst = page_address(pages->pages[i].linux_page);
		size_t n = min_t(size_t, size, PAGE_SIZE);

		memcpy(dst, data, n);
		dma_sync_single_for_device(pages->dev, dma, PAGE_SIZE,
					   DMA_TO_DEVICE);

		data += n;
		size -= n;
	}

	return 0;
}
