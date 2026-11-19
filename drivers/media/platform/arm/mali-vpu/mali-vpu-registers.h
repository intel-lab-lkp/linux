/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_REGISTERS_H__
#define __MALI_VPU_REGISTERS_H__

#include <linux/bits.h>
#include <linux/io.h>

/*
 * Mali VPU register offsets and bitfield definitions.
 *
 * Offsets are relative to the VPU MMIO base.
 */
#define MALI_VPU_REG_VERSION 0x00000
#define MALI_VPU_REG_NCORES  0x00008

/* Version register fields */
#define MALI_VPU_VERSION_PRODUCT_ID_MASK GENMASK(31, 16)
#define MALI_VPU_VERSION_REVISION_MASK   GENMASK(15, 8)
#define MALI_VPU_VERSION_PATCH_MASK      GENMASK(7, 0)
#define MALI_VPU_PID_SUFFIX_MASK         GENMASK(15, 12)

static inline u32 mali_vpu_read_reg(void __iomem *regs_base, u32 off)
{
	return readl(regs_base + off);
}

#endif /* __MALI_VPU_REGISTERS_H__ */
