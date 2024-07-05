/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CCU_H
#define _CORESIGHT_CCU_H

struct ccu_drvdata {
	void __iomem		*base;
	phys_addr_t		pbase;
	struct device		*dev;
	struct coresight_device	*csdev;
	spinlock_t		spin_lock;
	uint32_t		atid_offset;
};

#endif
