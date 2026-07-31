/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * AMD UFS host controller definitions.
 */
#ifndef _UFS_AMD_H
#define _UFS_AMD_H

#define REG_UFS_AMD_SQD         0x2200  /* SQ Doorbell base (runtime) */
#define REG_UFS_AMD_SQIS        0x2214  /* SQ Interrupt Status base */
#define REG_UFS_AMD_CQD         0x221C  /* CQ Doorbell base */
#define REG_UFS_AMD_CQIS        0x2224  /* CQ Interrupt Status base */
#define REG_UFS_AMD_MCQ_STRIDE  0x40    /* 64 bytes per queue */
#define REG_UFS_AMD_VS_CQES     0xE0    /* Vendor Specific: CQES CQ ID bitmap */

#endif /* !_UFS_AMD_H */
