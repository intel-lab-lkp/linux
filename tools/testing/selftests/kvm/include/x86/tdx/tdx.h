/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_TDX_TDX_H
#define SELFTESTS_TDX_TDX_H

#include <stdint.h>

/* MMIO direction */
#define MMIO_READ	0
#define MMIO_WRITE	1

uint64_t tdg_vp_vmcall_ve_request_mmio_write(uint64_t address, uint64_t size,
					     uint64_t data_in);

#endif // SELFTESTS_TDX_TDX_H
