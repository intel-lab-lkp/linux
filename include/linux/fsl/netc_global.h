/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/* Copyright 2024 NXP
 */
#ifndef __NETC_GLOBAL_H
#define __NETC_GLOBAL_H

#include <linux/io.h>

static inline u32 netc_read(void __iomem *reg)
{
	return ioread32(reg);
}

#ifdef ioread64
static inline u64 netc_read64(void __iomem *reg)
{
	return ioread64(reg);
}
#else
static inline u64 netc_read64(void __iomem *reg)
{
	u32 low, high;
	u64 val;

	low = ioread32(reg);
	high = ioread32(reg + 4);

	val = (u64)high << 32 | low;

	return val;
}
#endif

static inline void netc_write(void __iomem *reg, u32 val)
{
	iowrite32(val, reg);
}

#endif
