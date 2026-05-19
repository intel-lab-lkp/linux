/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_COMPUTE_BUS_H__
#define __QDA_COMPUTE_BUS_H__

#include <linux/device.h>

/*
 * Custom bus type for QDA compute context bank (CB) devices
 *
 * This bus type is used for manually created CB devices that represent
 * IOMMU context banks. The custom bus allows proper IOMMU configuration
 * and device management for these virtual devices.
 */
#ifdef CONFIG_DRM_ACCEL_QDA_COMPUTE_BUS
extern const struct bus_type qda_cb_bus_type;

struct device *create_qda_cb_device(struct device *parent_device, const char *name,
				    u64 dma_mask, struct device_node *of_node);
#else
static inline struct device *create_qda_cb_device(struct device *parent_device,
						  const char *name, u64 dma_mask,
						  struct device_node *of_node)
{
	return ERR_PTR(-ENODEV);
}
#endif

#endif /* __QDA_COMPUTE_BUS_H__ */
