// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * IOMMU API debug page alloc sanitizer
 */

#ifndef __LINUX_IOMMU_DEBUG_PAGEALLOC_H
#define __LINUX_IOMMU_DEBUG_PAGEALLOC_H

struct iommu_domain;

#ifdef CONFIG_IOMMU_DEBUG_PAGEALLOC

DECLARE_STATIC_KEY_FALSE(iommu_debug_initialized);

extern struct page_ext_operations page_iommu_debug_ops;

void __iommu_debug_map(struct iommu_domain *domain, phys_addr_t phys,
		       size_t size);
void __iommu_debug_unmap_begin(struct iommu_domain *domain,
			       unsigned long iova, size_t size);
void __iommu_debug_unmap_end(struct iommu_domain *domain,
			     unsigned long iova, size_t size, size_t unmapped);
void __iommu_debug_check_unmapped(const struct page *page, int numpages);

static inline void iommu_debug_map(struct iommu_domain *domain,
				   phys_addr_t phys, size_t size)
{
	if (static_branch_unlikely(&iommu_debug_initialized))
		__iommu_debug_map(domain, phys, size);
}

static inline void iommu_debug_unmap_begin(struct iommu_domain *domain,
					   unsigned long iova, size_t size)
{
	if (static_branch_unlikely(&iommu_debug_initialized))
		__iommu_debug_unmap_begin(domain, iova, size);
}

static inline void iommu_debug_unmap_end(struct iommu_domain *domain,
					 unsigned long iova, size_t size,
					 size_t unmapped)
{
	if (static_branch_unlikely(&iommu_debug_initialized))
		__iommu_debug_unmap_end(domain, iova, size, unmapped);
}

static inline void iommu_debug_check_unmapped(const struct page *page, int numpages)
{
	if (static_branch_unlikely(&iommu_debug_initialized))
		__iommu_debug_check_unmapped(page, numpages);
}

void iommu_debug_init(void);

#else
static inline void iommu_debug_map(struct iommu_domain *domain,
				   phys_addr_t phys, size_t size)
{
}

static inline void iommu_debug_unmap_begin(struct iommu_domain *domain,
					   unsigned long iova, size_t size)
{
}

static inline void iommu_debug_unmap_end(struct iommu_domain *domain,
					 unsigned long iova, size_t size,
					 size_t unmapped)
{
}

static inline void iommu_debug_init(void)
{
}

static inline void iommu_debug_check_unmapped(const struct page *page,
					      int numpages)
{
}

#endif /* CONFIG_IOMMU_DEBUG_PAGEALLOC */

#endif /* __LINUX_IOMMU_DEBUG_PAGEALLOC_H */
