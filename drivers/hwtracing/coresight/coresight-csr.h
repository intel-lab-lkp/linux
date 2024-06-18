/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CSR_H
#define _CORESIGHT_CSR_H

struct csr_drvdata {
	void __iomem		*base;
	phys_addr_t		pbase;
	struct device		*dev;
	struct coresight_device	*csdev;
	spinlock_t		spin_lock;
	uint32_t		atid_offset;
};

#if IS_ENABLED(CONFIG_CORESIGHT_CSR)
int csr_get_traceid(struct coresight_device *csdev);
#else
static inline int csr_get_traceid(struct coresight_device *csdev)
				{return -EINVAL; }
#endif
#endif
