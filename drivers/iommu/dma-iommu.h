/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014-2015 ARM Ltd.
 */
#ifndef __DMA_IOMMU_H
#define __DMA_IOMMU_H

#include <linux/iommu.h>

/*
 * Both dma-iommu.c and arch/arm/mm/dma-mapping.c implement functions to set up
 * IOMMU_DOMAIN_DMA default domains. Only one provider can be compiled in at a
 * time.
 */
#if defined(CONFIG_ARM_DMA_USE_IOMMU)

#include <asm/dma-iommu.h>

#elif defined(CONFIG_IOMMU_DMA)

void iommu_setup_dma_ops(struct device *dev, struct iommu_domain *domain);
void iommu_teardown_dma_ops(struct device *dev);

int iommu_get_dma_cookie(struct iommu_domain *domain);
void iommu_put_dma_cookie(struct iommu_domain *domain);

#else

static inline void iommu_setup_dma_ops(struct device *dev,
				       struct iommu_domain *domain)
{
}

static inline void iommu_teardown_dma_ops(struct device *dev)
{
}

static inline int iommu_get_dma_cookie(struct iommu_domain *domain)
{
	return -ENODEV;
}

static inline void iommu_put_dma_cookie(struct iommu_domain *domain)
{
}

#endif

#ifdef CONFIG_IOMMU_DMA

void iommu_put_msi_cookie(struct iommu_domain *domain);

int iommu_dma_init_fq(struct iommu_domain *domain);

void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list);

int iommu_dma_sw_msi(struct iommu_domain *domain, struct msi_desc *desc,
		     phys_addr_t msi_addr);

extern bool iommu_dma_forcedac;

#else /* CONFIG_IOMMU_DMA */

static inline int iommu_dma_init_fq(struct iommu_domain *domain)
{
	return -EINVAL;
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
