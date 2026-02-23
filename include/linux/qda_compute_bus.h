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
extern struct bus_type qda_cb_bus_type;
#endif

#endif /* __QDA_COMPUTE_BUS_H__ */
