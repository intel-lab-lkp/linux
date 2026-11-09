/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef AMDGPU_CWSR_H
#define AMDGPU_CWSR_H

#include <linux/types.h>

struct amdgpu_bo;
struct amdgpu_bo_va;
struct amdgpu_device;
struct amdgpu_vm;

/**
 * struct amdgpu_cwsr_obj - CWSR (Compute Wave Save Restore) buffer tracking
 * @bo: Buffer object for CWSR area
 * @bo_va: Buffer object virtual address mapping
 */
struct amdgpu_cwsr_trap_obj {
	uint64_t tma_gpu_va_addr;
	uint64_t tba_gpu_va_addr;

	struct amdgpu_bo *tma_bo;
	struct amdgpu_bo_va *tba_va;
	struct amdgpu_bo_va *tma_va;
	void *tma_cpu_addr;
};

struct amdgpu_cwsr_info {
	/* cwsr isa */
	struct amdgpu_bo *isa_bo;
	const void *isa_buf;
	uint32_t isa_sz;
	/* cwsr size info per XCC*/
	uint32_t xcc_ctl_stack_sz;
	uint32_t xcc_dbg_mem_sz;
	uint32_t xcc_cwsr_sz;
};

struct amdgpu_cwsr_params {
	uint64_t ctx_save_area_address;
	/* cwsr size info */
	uint32_t cwsr_sz;
};

int amdgpu_cwsr_init(struct amdgpu_device *adev);
void amdgpu_cwsr_fini(struct amdgpu_device *adev);

int amdgpu_cwsr_alloc(struct amdgpu_device *adev, struct amdgpu_vm *vm,
		      struct amdgpu_cwsr_trap_obj **cwsr_obj);
void amdgpu_cwsr_free(struct amdgpu_device *adev, struct amdgpu_vm *vm,
		      struct amdgpu_cwsr_trap_obj **cwsr_obj);
static inline bool amdgpu_cwsr_is_enabled(struct amdgpu_device *adev)
{
	return adev->cwsr_info != NULL;
}

uint32_t amdgpu_cwsr_size_needed(struct amdgpu_device *adev, int num_xcc);
int amdgpu_cwsr_validate_params(struct amdgpu_device *adev,
				struct amdgpu_cwsr_params *cwsr_params,
				int num_xcc);
#endif
