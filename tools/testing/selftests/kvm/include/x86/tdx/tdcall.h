/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted from arch/x86/include/asm/shared/tdx.h */

#ifndef SELFTESTS_TDX_TDCALL_H
#define SELFTESTS_TDX_TDCALL_H

#include <linux/bits.h>

#define TDX_TDCALL_HAS_OUTPUT BIT(0)

#ifndef __ASSEMBLY__

#include <linux/types.h>

/*
 * Used in __tdx_tdcall() to pass down and get back registers' values of
 * the TDCALL instruction when requesting services from the VMM.
 *
 * This is a software only structure and not part of the TDX module/VMM ABI.
 */
struct tdx_tdcall_args {
	u64 r10;
	u64 r11;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
};

/* Used to request services from the VMM */
u64 __tdx_tdcall(struct tdx_tdcall_args *args, unsigned long flags);

#endif // __ASSEMBLY__
#endif // SELFTESTS_TDX_TDCALL_H
