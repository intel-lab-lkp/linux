// SPDX-License-Identifier: GPL-2.0-only

#include "tdx/tdcall.h"
#include "tdx/tdx.h"

#define TDG_VP_VMCALL 0

#define TDG_VP_VMCALL_VE_REQUEST_MMIO	48

uint64_t tdg_vp_vmcall_ve_request_mmio_write(uint64_t address, uint64_t size,
					     uint64_t data_in)
{
	struct tdx_tdcall_args args = {
		.r11 = TDG_VP_VMCALL_VE_REQUEST_MMIO,
		.r12 = size,
		.r13 = MMIO_WRITE,
		.r14 = address,
		.r15 = data_in,
	};

	return __tdx_tdcall(&args, 0);
}
