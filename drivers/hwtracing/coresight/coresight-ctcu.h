/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CTCU_H
#define _CORESIGHT_CTCU_H
#include "coresight-trace-id.h"

/* Maximum number of supported sink devices for a single CTCU in current projects. */
#define ATID_MAX_NUM 	2

struct ctcu_drvdata {
	void __iomem		*base;
	struct clk		*apb_clk;
	phys_addr_t		pbase;
	struct device		*dev;
	struct coresight_device	*csdev;
	raw_spinlock_t		spin_lock;
	u32			atid_offset[ATID_MAX_NUM];
	/* refcnt for each traceid of each sink */
	u8			traceid_refcnt[ATID_MAX_NUM][CORESIGHT_TRACE_ID_RES_TOP];
};
#endif
