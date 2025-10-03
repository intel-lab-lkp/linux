// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * IOMMU API santaizers and debug
 */
#include <linux/atomic.h>
#include <linux/iommu.h>
#include <linux/iommu-debug.h>
#include <linux/kernel.h>
#include <linux/page_ext.h>

static bool needed;
static DEFINE_STATIC_KEY_FALSE(iommu_debug_initialized);

struct iommu_debug_metadate {
	atomic_t ref;
};

static __init bool need_iommu_debug(void)
{
	return needed;
}

struct page_ext_operations page_iommu_debug_ops = {
	.size = sizeof(struct iommu_debug_metadate),
	.need = need_iommu_debug,
};

static struct page_ext *get_iommu_page_ext(phys_addr_t phys)
{
	struct page *page = phys_to_page(phys);
	struct page_ext *page_ext = page_ext_get(page);

	return page_ext;
}

static struct iommu_debug_metadate *get_iommu_data(struct page_ext *page_ext)
{
	return page_ext_data(page_ext, &page_iommu_debug_ops);
}

static void iommu_debug_inc_page(phys_addr_t phys)
{
	struct page_ext *page_ext = get_iommu_page_ext(phys);
	struct iommu_debug_metadate *d = get_iommu_data(page_ext);

	WARN_ON(atomic_inc_return(&d->ref) <= 0);
	page_ext_put(page_ext);
}

static void iommu_debug_dec_page(phys_addr_t phys)
{
	struct page_ext *page_ext = get_iommu_page_ext(phys);
	struct iommu_debug_metadate *d = get_iommu_data(page_ext);

	WARN_ON(atomic_dec_return(&d->ref) < 0);
	page_ext_put(page_ext);
}

/*
 * IOMMU pages size might not match the CPU page size, in that case, we use
 * the smallest IOMMU page size to refcount the pages in the vmemap.
 * That's is important as both map and unmap has to use the same page size
 * to update the refcount to avoid double counting the same page.
 * And as we can't know from iommu_unmap() what was the original page size
 * used for map, we just use the minimum supported one for both.
 */
static size_t iommu_debug_page_size(struct iommu_domain *domain)
{
	return 1UL << __ffs(domain->pgsize_bitmap);
}

void iommu_debug_map(struct iommu_domain *domain, phys_addr_t phys, size_t size)
{
	size_t off;
	size_t page_size = iommu_debug_page_size(domain);

	if (!static_branch_likely(&iommu_debug_initialized))
		return;

	for (off = 0 ; off < size ; off += page_size) {
		if (!pfn_valid(__phys_to_pfn(phys + off)))
			continue;
		iommu_debug_inc_page(phys + off);
	}
}

void iommu_debug_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	size_t off;
	size_t page_size = iommu_debug_page_size(domain);

	if (!static_branch_likely(&iommu_debug_initialized))
		return;

	for (off = 0 ; off < size ; off += page_size) {
		phys_addr_t phys = iommu_iova_to_phys(domain, iova + off);

		if (!phys || !pfn_valid(__phys_to_pfn(phys + off)))
			continue;

		iommu_debug_dec_page(phys);
	}
}

void iommu_debug_remap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	size_t off;
	size_t page_size = iommu_debug_page_size(domain);

	if (!static_branch_likely(&iommu_debug_initialized))
		return;

	for (off = 0 ; off < size ; off += page_size) {
		phys_addr_t phys = iommu_iova_to_phys(domain, iova + off);

		if (!phys || !pfn_valid(__phys_to_pfn(phys + off)))
			continue;

		iommu_debug_inc_page(phys);
	}
}

void iommu_debug_init(void)
{
	if (!needed)
		return;

	pr_info("iommu: Debugging page allocations, expect overhead or disable iommu.debug_pagealloc");
	static_branch_enable(&iommu_debug_initialized);
}

static int __init iommu_debug_pagealloc(char *str)
{
	return kstrtobool(str, &needed);
}
early_param("iommu.debug_pagealloc", iommu_debug_pagealloc);
