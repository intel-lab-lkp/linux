/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_HW_REGS_H__
#define __PANTHOR_HW_REGS_H__

#define GPU_ID						0x0
#define   GPU_ARCH_MAJOR(x)				((x) >> 28)
#define   GPU_ARCH_MINOR(x)				(((x) & GENMASK(27, 24)) >> 24)
#define   GPU_ARCH_REV(x)				(((x) & GENMASK(23, 20)) >> 20)
#define   GPU_PROD_MAJOR(x)				(((x) & GENMASK(19, 16)) >> 16)
#define   GPU_VER_MAJOR(x)				(((x) & GENMASK(15, 12)) >> 12)
#define   GPU_VER_MINOR(x)				(((x) & GENMASK(11, 4)) >> 4)
#define   GPU_VER_STATUS(x)				((x) & GENMASK(3, 0))

#endif /* __PANTHOR_HW_REGS_H__ */
