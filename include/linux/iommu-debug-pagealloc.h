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
void __iommu_debug_unmap(struct iommu_domain *domain, unsigned long iova,
			 size_t size);
void __iommu_debug_remap(struct iommu_domain *domain, unsigned long iova,
			 size_t size);

static inline void iommu_debug_map(struct iommu_domain *domain,
				   phys_addr_t phys, size_t size)
{
	if (static_branch_unlikely(&iommu_debug_initialized))
		__iommu_debug_map(domain, phys, size);
}

static inline void iommu_debug_unmap(struct iommu_domain *domain,
				     unsigned long iova, size_t size)
{
	if (static_branch_unlikely(&iommu_debug_initialized))
		__iommu_debug_unmap(domain, iova, size);
}

static inline void iommu_debug_remap(struct iommu_domain *domain,
				     unsigned long iova, size_t size)
{
	if (static_branch_unlikely(&iommu_debug_initialized))
		__iommu_debug_remap(domain, iova, size);
}

void iommu_debug_init(void);

#else
static inline void iommu_debug_map(struct iommu_domain *domain,
				   phys_addr_t phys, size_t size)
{
}

static inline void iommu_debug_unmap(struct iommu_domain *domain,
				     unsigned long iova, size_t size)
{
}

static inline void iommu_debug_remap(struct iommu_domain *domain,
				     unsigned long iova, size_t size)
{
}

static inline void iommu_debug_init(void)
{
}

#endif /* CONFIG_IOMMU_DEBUG_PAGEALLOC */

#endif /* __LINUX_IOMMU_DEBUG_PAGEALLOC_H */
