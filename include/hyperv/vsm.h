/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common definitions shared by generic and arch code for enabling VTL1
 * and the Virtual Secure Mode (VSM) framework on Microsoft Hyper-V.
 *
 * Copyright (c) 2025-2026, Microsoft Corporation.
 *
 * Author: Thara Gopinath <tgopinath@linux.microsoft.com>
 */

#ifndef _HYPERV_VSM_H
#define _HYPERV_VSM_H

/*
 * Size of memory that is initially mapped for the secure kernel by the
 * VTL0-side loader. The secure kernel image itself may be larger than
 * this and map additional memory on its own.
 */
#define VSM_SK_INITIAL_MAP_SIZE		(16 * 1024 * 1024)

#endif /* _HYPERV_VSM_H */
