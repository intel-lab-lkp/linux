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

#include <linux/types.h>

/*
 * Size of memory that is initially mapped for the secure kernel by the
 * VTL0-side loader. The secure kernel image itself may be larger than
 * this and map additional memory on its own.
 */
#define VSM_SK_INITIAL_MAP_SIZE		(16 * 1024 * 1024)

/*
 * Argument block passed from VTL0 to VTL1 across a vtlcall. Layout is
 * shared with the arch-specific assembly trampoline that marshals these
 * into registers.
 */
struct hv_vtlcall_param {
	u64	a0;
	u64	a1;
	u64	a2;
	u64	a3;
} __packed;

#ifdef CONFIG_HYPERV_VSM
s64 hv_vsm_vtlcall(struct hv_vtlcall_param *args);
void hv_vsm_init_vtlcall(u64 vtl_call_offset);
#else
static inline s64 hv_vsm_vtlcall(struct hv_vtlcall_param *args) { return 0; }
static inline void hv_vsm_init_vtlcall(u64 vtl_call_offset) {}
#endif

#endif /* _HYPERV_VSM_H */
