// SPDX-License-Identifier: GPL-2.0
#define COMPILE_OFFSETS

#include <linux/kbuild.h>

#include "tdx/tdcall.h"

static void __attribute__((used)) common(void)
{
	OFFSET(TDX_TDCALL_R10, tdx_tdcall_args, r10);
	OFFSET(TDX_TDCALL_R11, tdx_tdcall_args, r11);
	OFFSET(TDX_TDCALL_R12, tdx_tdcall_args, r12);
	OFFSET(TDX_TDCALL_R13, tdx_tdcall_args, r13);
	OFFSET(TDX_TDCALL_R14, tdx_tdcall_args, r14);
	OFFSET(TDX_TDCALL_R15, tdx_tdcall_args, r15);
}
