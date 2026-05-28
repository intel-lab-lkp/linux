/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_GPU_DISCOVER_REGS_H__
#define __PANTHOR_GPU_DISCOVER_REGS_H__

#define GPU_WIDE_ID					0x0
#define   GPU_WIDE_COMPAT				0xF
#define   GPU_WIDE_ARCH_MAJOR(x)			(((x) & GENMASK(63, 56)) >> 56)
#define   GPU_WIDE_ARCH_MINOR(x)			(((x) & GENMASK(55, 48)) >> 48)
#define   GPU_WIDE_ARCH_REV(x)				(((x) & GENMASK(47, 40)) >> 40)
#define   GPU_WIDE_PROD_MAJOR(x)			(((x) & GENMASK(39, 32)) >> 32)
#define   GPU_WIDE_VER_MAJOR(x)				(((x) & GENMASK(23, 16)) >> 16)
#define   GPU_WIDE_VER_MINOR(x)				(((x) & GENMASK(15, 8)) >> 8)
#define   GPU_WIDE_VER_STATUS(x)			((x) & GENMASK(7, 0))

#endif /* __PANTHOR_GPU_DISCOVER_REGS_H__ */
