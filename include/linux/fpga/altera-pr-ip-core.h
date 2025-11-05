/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Altera Partial Reconfiguration IP Core
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Based on socfpga-a10.c Copyright (C) 2015-2016 Altera Corporation
 *  by Alan Tull <atull@opensource.altera.com>
 */

#ifndef _ALT_PR_IP_CORE_H
#define _ALT_PR_IP_CORE_H
#include <linux/io.h>
#include <asm/errno.h>

#if IS_ENABLED(CONFIG_ALTERA_PR_IP_CORE)
int alt_pr_register(struct device *dev, void __iomem *reg_base);
#else
static inline int alt_pr_register(struct device *dev, void __iomem *reg_base)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _ALT_PR_IP_CORE_H */
