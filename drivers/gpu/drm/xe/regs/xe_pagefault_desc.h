/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_PAGEFAULT_DESC_H_
#define _XE_PAGEFAULT_DESC_H_

#include <linux/bits.h>
#include <linux/types.h>

struct xe_guc_pagefault_desc {
	u32 dw0;
#define PFD_FAULT_LEVEL		GENMASK(2, 0)
#define PFD_SRC_ID		GENMASK(10, 3)
#define PFD_RSVD_0		GENMASK(17, 11)
#define XE2_PFD_TRVA_FAULT	BIT(18)
#define PFD_ENG_INSTANCE	GENMASK(24, 19)
#define PFD_ENG_CLASS		GENMASK(27, 25)
#define PFD_PDATA_LO		GENMASK(31, 28)

	u32 dw1;
#define PFD_PDATA_HI		GENMASK(11, 0)
#define PFD_PDATA_HI_SHIFT	4
#define PFD_ASID		GENMASK(31, 12)

	u32 dw2;
#define PFD_ACCESS_TYPE		GENMASK(1, 0)
#define PFD_FAULT_TYPE		GENMASK(3, 2)
#define PFD_VFID		GENMASK(9, 4)
#define PFD_RSVD_1		GENMASK(11, 10)
#define PFD_VIRTUAL_ADDR_LO	GENMASK(31, 12)
#define PFD_VIRTUAL_ADDR_LO_SHIFT 12

	u32 dw3;
#define PFD_VIRTUAL_ADDR_HI	GENMASK(31, 0)
#define PFD_VIRTUAL_ADDR_HI_SHIFT 32
} __packed;

#define FLT_ACCESS_TYPE_READ		0u
#define FLT_ACCESS_TYPE_WRITE		1u
#define FLT_ACCESS_TYPE_ATOMIC		2u
#define FLT_ACCESS_TYPE_RESERVED	3u

#define FLT_TYPE_NOT_PRESENT_FAULT		0u
#define FLT_TYPE_WRITE_ACCESS_VIOLATION		1u
#define FLT_TYPE_ATOMIC_ACCESS_VIOLATION	2u

#endif
