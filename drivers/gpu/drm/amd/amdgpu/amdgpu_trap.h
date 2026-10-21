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

#ifndef AMDGPU_TRAP_H
#define AMDGPU_TRAP_H

#include <linux/types.h>

struct amdgpu_bo;
struct amdgpu_bo_va;
struct amdgpu_device;
struct amdgpu_vm;

/**
 * struct amdgpu_trap_obj - per-GPUVM CWSR trap-handler state
 * @tma_gpu_va_addr: GPU virtual address of the first-level TMA
 * @tba_gpu_va_addr: GPU virtual address of the first-level TBA
 * @tma_bo: buffer object backing the first-level TMA
 * @tba_va: VM mapping of the first-level TBA
 * @tma_va: VM mapping of the first-level TMA
 */
struct amdgpu_trap_obj {
	uint64_t tma_gpu_va_addr;
	uint64_t tba_gpu_va_addr;

	struct amdgpu_bo *tma_bo;
	struct amdgpu_bo_va *tba_va;
	struct amdgpu_bo_va *tma_va;
};

struct amdgpu_trap_info {
	/* cwsr isa */
	struct amdgpu_bo *isa_bo;
	const void *isa_buf;
	uint32_t isa_sz;

	/* cwsr size info per XCC */
	struct {
		uint32_t xcc_ctl_stack_sz;
		uint32_t xcc_dbg_mem_sz;
		uint32_t xcc_cwsr_sz;
	} cwsr;
};

struct amdgpu_trap_params {
	uint64_t ctx_save_area_address;

	/* cwsr size info */
	uint32_t cwsr_sz;
};

struct amdgpu_trap_usr_addr {
	uint64_t addr;
	uint32_t size;
};

int amdgpu_trap_init(struct amdgpu_device *adev);
void amdgpu_trap_fini(struct amdgpu_device *adev);

int amdgpu_trap_alloc(struct amdgpu_device *adev, struct amdgpu_vm *vm,
		      struct amdgpu_trap_obj **trap_obj);

void amdgpu_trap_free(struct amdgpu_device *adev,
		      struct amdgpu_vm *vm,
		      struct amdgpu_trap_obj **trap_obj);

static inline bool amdgpu_trap_is_enabled(struct amdgpu_device *adev)
{
	return adev->trap_info;
}

uint32_t amdgpu_trap_size_needed(struct amdgpu_device *adev, int num_xcc);

int amdgpu_trap_validate_params(struct amdgpu_device *adev,
				struct amdgpu_trap_params *trap_params,
				int num_xcc);

static inline bool amdgpu_trap_has_dbg_wa(struct amdgpu_device *adev)
{
	uint32_t gc_ver = amdgpu_ip_version(adev, GC_HWIP, 0);

	return gc_ver >= IP_VERSION(11, 0, 0) &&
	       gc_ver <= IP_VERSION(11, 0, 3);
}

/**
 * amdgpu_trap_vm_set_l2_trap - install the second-level trap handler
 * @adev: AMDGPU device
 * @vm: GPUVM owning the first-level trap state
 * @trap_obj: first-level trap object
 * @tba_va: GPU virtual address of the second-level TBA
 * @tba_sz: size of the second-level TBA
 * @tma_va: GPU virtual address of the second-level TMA
 * @tma_sz: size of the second-level TMA
 *
 * The second-level trap handler is configured per GPUVM. The descriptor is
 * stored in the per-GPUVM first-level TMA.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int amdgpu_trap_vm_set_l2_trap(struct amdgpu_device *adev,
			       struct amdgpu_vm *vm,
			       struct amdgpu_trap_obj *trap_obj,
			       u64 tba_va, uint32_t tba_sz,
			       u64 tma_va, uint32_t tma_sz);

/**
 * amdgpu_trap_vm_clear_l2_trap - clear the second-level trap handler
 * @adev: AMDGPU device
 * @vm: GPUVM owning the first-level trap state
 * @trap_obj: first-level trap object
 *
 * Return: 0 on success or a negative error code on failure.
 */
int amdgpu_trap_vm_clear_l2_trap(struct amdgpu_device *adev,
				 struct amdgpu_vm *vm,
				 struct amdgpu_trap_obj *trap_obj);

int amdgpu_trap_set_trap_debug_flag(struct amdgpu_device *adev,
				    struct amdgpu_vm *vm,
				    struct amdgpu_trap_obj *trap_obj,
				    bool enabled);

#endif
