/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_DEVICE_IO_H__
#define __PANTHOR_DEVICE_IO_H__

#include <linux/atomic.h>
#include <linux/io.h>

static inline void gpu_write(void __iomem *iomem, u32 reg, u32 data)
{
	writel(data, iomem + reg);
}

static inline u32 gpu_read(void __iomem *iomem, u32 reg)
{
	return readl(iomem + reg);
}

static inline u32 gpu_read_relaxed(void __iomem *iomem, u32 reg)
{
	return readl_relaxed(iomem + reg);
}

static inline void gpu_write64(void __iomem *iomem, u32 reg, u64 data)
{
	gpu_write(iomem, reg, lower_32_bits(data));
	gpu_write(iomem, reg + 4, upper_32_bits(data));
}

static inline u64 gpu_read64(void __iomem *iomem, u32 reg)
{
	return (gpu_read(iomem, reg) | ((u64)gpu_read(iomem, reg + 4) << 32));
}

static inline u64 gpu_read64_relaxed(void __iomem *iomem, u32 reg)
{
	return (gpu_read_relaxed(iomem, reg) |
		((u64)gpu_read_relaxed(iomem, reg + 4) << 32));
}

static inline u64 gpu_read64_counter(void __iomem *iomem, u32 reg)
{
	u32 lo, hi1, hi2;
	do {
		hi1 = gpu_read(iomem, reg + 4);
		lo = gpu_read(iomem, reg);
		hi2 = gpu_read(iomem, reg + 4);
	} while (hi1 != hi2);
	return lo | ((u64)hi2 << 32);
}

#define gpu_read_poll_timeout(iomem, reg, val, cond, delay_us, timeout_us)	\
	read_poll_timeout(gpu_read, val, cond, delay_us, timeout_us, false,	\
			  iomem, reg)

#define gpu_read_poll_timeout_atomic(iomem, reg, val, cond, delay_us,		\
				     timeout_us)				\
	read_poll_timeout_atomic(gpu_read, val, cond, delay_us, timeout_us,	\
				 false, iomem, reg)

#define gpu_read64_poll_timeout(iomem, reg, val, cond, delay_us, timeout_us)	\
	read_poll_timeout(gpu_read64, val, cond, delay_us, timeout_us, false,	\
			  iomem, reg)

#define gpu_read64_poll_timeout_atomic(iomem, reg, val, cond, delay_us,		\
				       timeout_us)				\
	read_poll_timeout_atomic(gpu_read64, val, cond, delay_us, timeout_us,	\
				 false, iomem, reg)

#define gpu_read_relaxed_poll_timeout_atomic(iomem, reg, val, cond, delay_us,	\
					     timeout_us)			\
	read_poll_timeout_atomic(gpu_read_relaxed, val, cond, delay_us,		\
				 timeout_us, false, iomem, reg)

#define gpu_read64_relaxed_poll_timeout(iomem, reg, val, cond, delay_us,	\
					timeout_us)				\
	read_poll_timeout(gpu_read64_relaxed, val, cond, delay_us, timeout_us,	\
			  false, iomem, reg)

#endif /* __PANTHOR_DEVICE_IO_H__ */
