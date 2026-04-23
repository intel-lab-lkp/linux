/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _LINUX_DMA_CONTEXT_BUS_H
#define _LINUX_DMA_CONTEXT_BUS_H

#include <linux/device.h>

#ifdef CONFIG_DMA_CONTEXT_BUS
extern const struct bus_type dma_context_bus_type;

struct device *create_dma_context_bus_device(struct device *parent_device,
					     struct device_node *of_node,
					     u64 dma_mask, const u32 *iommu_f_id);
#else
static inline struct device *create_dma_context_bus_device(struct device *parent_device,
							   struct device_node *of_node,
							   u64 dma_mask, const u32 *iommu_f_id)
{
	return NULL;
}
#endif

#endif /* _LINUX_DMA_CONTEXT_BUS_H */
