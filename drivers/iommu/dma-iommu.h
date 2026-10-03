/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014-2015 ARM Ltd.
 */
#ifndef __DMA_IOMMU_H
#define __DMA_IOMMU_H

#include <linux/iommu.h>

struct dma_pmd_window;
struct iova_domain;

#ifdef CONFIG_IOMMU_DMA

void iommu_setup_dma_ops(struct device *dev, struct iommu_domain *domain);

int iommu_get_dma_cookie(struct iommu_domain *domain);
void iommu_put_dma_cookie(struct iommu_domain *domain);
void iommu_put_msi_cookie(struct iommu_domain *domain);

int iommu_dma_init_fq(struct iommu_domain *domain);

void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list);

int iommu_dma_sw_msi(struct iommu_domain *domain, struct msi_desc *desc,
		     phys_addr_t msi_addr);

int dma_info_to_prot(enum dma_data_direction dir, bool coherent, unsigned long attrs);

struct iova_domain *dma_pmd_dma_iovad(struct iommu_domain *domain);
struct dma_pmd_window *dma_pmd_dma_window(struct iommu_domain *domain);

extern bool iommu_dma_forcedac;

#else /* CONFIG_IOMMU_DMA */

static inline void iommu_setup_dma_ops(struct device *dev,
				       struct iommu_domain *domain)
{
}

static inline int iommu_dma_init_fq(struct iommu_domain *domain)
{
	return -EINVAL;
}

static inline int iommu_get_dma_cookie(struct iommu_domain *domain)
{
	return -ENODEV;
}

static inline void iommu_put_dma_cookie(struct iommu_domain *domain)
{
}

static inline void iommu_put_msi_cookie(struct iommu_domain *domain)
{
}

static inline void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)
{
}

static inline int iommu_dma_sw_msi(struct iommu_domain *domain,
				   struct msi_desc *desc, phys_addr_t msi_addr)
{
	return -ENODEV;
}

#endif	/* CONFIG_IOMMU_DMA */
#endif	/* __DMA_IOMMU_H */
