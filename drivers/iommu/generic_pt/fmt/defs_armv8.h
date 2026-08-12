/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2026, NVIDIA CORPORATION & AFFILIATES
 *
 * VMSAv8-64 translation table in AArch64 mode
 *
 */
#ifndef __GENERIC_PT_FMT_DEFS_ARMV8_H
#define __GENERIC_PT_FMT_DEFS_ARMV8_H

#include <linux/generic_pt/common.h>
#include <linux/types.h>

typedef u64 pt_vaddr_t;
typedef u64 pt_oaddr_t;

struct armv8pt_write_attrs {
	u64 descriptor_bits;
	gfp_t gfp;
};
#define pt_write_attrs armv8pt_write_attrs

#endif
