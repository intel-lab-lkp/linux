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
#define MALI_VPU_REG_VERSION  0x00000
#define MALI_VPU_REG_ENABLE   0x00004
#define MALI_VPU_REG_NCORES   0x00008
#define MALI_VPU_REG_NLSID    0x0000C
#define MALI_VPU_REG_JOBQUEUE 0x00014
#define MALI_VPU_REG_CLKFORCE 0x00024
#define MALI_VPU_REG_RESET    0x00050

#define MALI_VPU_LSID_BASE    0x00200
#define MALI_VPU_LSID_STRIDE  0x40

/*
 * Per-LSID registers.
 * Address = MALI_VPU_LSID_BASE + (MALI_VPU_LSID_STRIDE * lsid) + per-LSID offset
 * Each LSID has its own set of these registers, which are identical in layout.
 */
#define MALI_VPU_REG_LSID_CTRL      0x00
#define MALI_VPU_REG_LSID_MMU_CTRL  0x04
#define MALI_VPU_REG_LSID_NPROT     0x08
#define MALI_VPU_REG_LSID_ALLOC     0x0C
#define MALI_VPU_REG_LSID_FLUSH_ALL 0x10
#define MALI_VPU_REG_LSID_SCHED     0x14
#define MALI_VPU_REG_LSID_TERMINATE 0x18
#define MALI_VPU_REG_LSID_IRQVE     0x1C
#define MALI_VPU_REG_LSID_IRQHOST   0x20
#define MALI_VPU_REG_LSID_BUSATTR_0 0x30
#define MALI_VPU_REG_LSID_BUSATTR_1 0x34
#define MALI_VPU_REG_LSID_BUSATTR_2 0x38
#define MALI_VPU_REG_LSID_BUSATTR_3 0x3C

/* Version register fields */
#define MALI_VPU_VERSION_PRODUCT_ID_MASK GENMASK(31, 16)
#define MALI_VPU_VERSION_REVISION_MASK   GENMASK(15, 8)
#define MALI_VPU_VERSION_PATCH_MASK      GENMASK(7, 0)
#define MALI_VPU_PID_SUFFIX_MASK         GENMASK(15, 12)

/* JOBQUEUE register: 4 entries x 8 bits (LSID[3:0], NCORES-1[7:4]) */
#define MALI_VPU_JOBQUEUE_LSID_BITS   4
#define MALI_VPU_JOBQUEUE_NCORES_BITS 4
#define MALI_VPU_JOBQUEUE_JOB_STRIDE  8
#define MALI_VPU_JOBQUEUE_JOB_INVALID 0xf
#define MALI_VPU_JOBQUEUE_NJOBS       4

/* IRQVE contains one interrupt bit per LSID. */
#define MALI_VPU_IRQVE_NUM_BITS 32

/* CTRL register fields */
#define MALI_VPU_CTRL_MAXCORES GENMASK(11, 8)

#define MALI_VPU_ALLOC_FREE          0
#define MALI_VPU_ALLOC_NON_PROTECTED 1
#define MALI_VPU_ALLOC_PROTECTED     2

static inline u32 mali_vpu_read_reg(void __iomem *regs_base, u32 off)
{
	return readl(regs_base + off);
}

static inline void mali_vpu_write_reg(void __iomem *regs_base, u32 off, u32 val)
{
	writel(val, regs_base + off);
}

#endif /* __MALI_VPU_REGISTERS_H__ */
