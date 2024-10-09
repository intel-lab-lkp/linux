/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CTCU_H
#define _CORESIGHT_CTCU_H

#define ATID_MAX_NUM 5

struct ctcu_drvdata {
	void __iomem		*base;
	struct clk		*apb_clk;
	phys_addr_t		pbase;
	struct device		*dev;
	struct coresight_device	*csdev;
	spinlock_t		spin_lock;
	uint32_t		atid_offset[ATID_MAX_NUM];
};

#endif
