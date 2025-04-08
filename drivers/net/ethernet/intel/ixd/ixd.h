/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2025 Intel Corporation */

#ifndef _IXD_H_
#define _IXD_H_

#include <net/libeth/pci.h>

/**
 * struct ixd_adapter - Data structure representing a CPF
 * @hw: Device access data
 */
struct ixd_adapter {
	struct libeth_mmio_info hw;
};

/**
 * ixd_to_dev - Get the corresponding device struct from an adapter
 * @adapter: PCI device driver-specific private data
 */
static inline struct device *ixd_to_dev(struct ixd_adapter *adapter)
{
	return &adapter->hw.pdev->dev;
}

#endif /* _IXD_H_ */
