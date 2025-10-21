/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _QCOM_TGU_H
#define _QCOM_TGU_H

/* Register addresses */
#define TGU_CONTROL 0x0000

/**
 * struct tgu_drvdata - Data structure for a TGU (Trigger Generator Unit)
 * @base: Memory-mapped base address of the TGU device
 * @dev: Pointer to the associated device structure
 * @csdev: Pointer to the associated coresight device
 * @spinlock: Spinlock for handling concurrent access
 * @enable: Flag indicating whether the TGU device is enabled
 *
 * This structure defines the data associated with a TGU device,
 * including its base address, device pointers, clock, spinlock for
 * synchronization, trigger data pointers, maximum limits for various
 * trigger-related parameters, and enable status.
 */
struct tgu_drvdata {
	void __iomem *base;
	struct device *dev;
	struct coresight_device *csdev;
	spinlock_t spinlock;
	bool enable;
};

#endif
