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
#include <linux/iosys-map.h>

#include <drm/ttm/ttm_bo.h>
#include <drm/drm_exec.h>

#include "amdgpu.h"
#include "cwsr_trap_handler.h"
#include "amdgpu_trap.h"

/*
 * First-level TMA layout used by the second-level trap-handler ABI.
 * The trap-handler ISA reads L2 TBA at offset 0 and L2 TMA at offset 8.
 */
#define AMDGPU_TRAP_L2_TBA_INDEX	0
#define AMDGPU_TRAP_L2_TMA_INDEX	1
#define AMDGPU_TRAP_DEBUG_FLAG_INDEX	2

extern int cwsr_enable;

#define AMDGPU_TRAP_TBA_MAX_SIZE (2 * AMDGPU_GPU_PAGE_SIZE)
#define AMDGPU_TRAP_TMA_MAX_SIZE (AMDGPU_GPU_PAGE_SIZE)
#define AMDGPU_TRAP_TMA_OFFSET (AMDGPU_TRAP_TBA_MAX_SIZE)

#define SGPR_SIZE_PER_CU 0x4000
#define LDS_SIZE_PER_CU 0x10000
#define HWREG_SIZE_PER_CU 0x1000
#define DEBUGGER_BYTES_ALIGN 64
#define DEBUGGER_BYTES_PER_WAVE 32
#define SIZEOF_HSA_USER_CONTEXT_SAVE_AREA_HEADER 40

enum amdgpu_trap_region {
	AMDGPU_TRAP_TBA,
	AMDGPU_TRAP_TMA,
};

static inline uint64_t amdgpu_trap_tba_vaddr(struct amdgpu_device *adev)
{
	uint64_t addr = AMDGPU_VA_RESERVED_TRAP_UQ_START(adev);

	addr = amdgpu_gmc_sign_extend(addr);

	return addr;
}

static inline bool amdgpu_trap_is_supported(struct amdgpu_device *adev)
{
	uint32_t gc_ver = amdgpu_ip_version(adev, GC_HWIP, 0);

	if (!cwsr_enable || gc_ver < IP_VERSION(9, 0, 1))
		return false;

	return true;
}

uint32_t amdgpu_trap_size_needed(struct amdgpu_device *adev, int num_xcc)
{
	if (!amdgpu_trap_is_enabled(adev))
		return 0;

	return num_xcc *
	       (adev->trap_info->cwsr.xcc_cwsr_sz + adev->trap_info->cwsr.xcc_dbg_mem_sz);
}

static void amdgpu_trap_cwsr_init_isa_details(struct amdgpu_device *adev,
					      struct amdgpu_trap_info *trap_info)
{
	uint32_t gc_ver = amdgpu_ip_version(adev, GC_HWIP, 0);

	if (gc_ver < IP_VERSION(9, 0, 1)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx8_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx8_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_gfx8_hex);
	} else if (gc_ver == IP_VERSION(9, 4, 1)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_arcturus_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_arcturus_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_arcturus_hex);
	} else if (gc_ver == IP_VERSION(9, 4, 2)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_aldebaran_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_aldebaran_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_aldebaran_hex);
	} else if (gc_ver == IP_VERSION(9, 4, 3) ||
		   gc_ver == IP_VERSION(9, 4, 4)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx9_4_3_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx9_4_3_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_gfx9_4_3_hex);
	} else if (gc_ver == IP_VERSION(9, 5, 0)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx9_5_0_hex) > PAGE_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx9_5_0_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_gfx9_5_0_hex);
	} else if (gc_ver < IP_VERSION(10, 1, 1)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx9_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx9_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_gfx9_hex);
	} else if (gc_ver < IP_VERSION(10, 3, 0)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_nv1x_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_nv1x_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_nv1x_hex);
	} else if (gc_ver < IP_VERSION(11, 0, 0)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx10_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx10_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_gfx10_hex);
	} else if (gc_ver < IP_VERSION(12, 0, 0)) {
		/* The gfx11 cwsr trap handler must fit inside a single page. */
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx11_hex) > PAGE_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx11_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_gfx11_hex);
	} else if (gc_ver < IP_VERSION(12, 1, 0)) {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx12_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx12_hex;
		trap_info->isa_sz = sizeof(cwsr_trap_gfx12_hex);
	} else if (gc_ver == IP_VERSION(12, 1, 0) &&
		   (adev->rev_id & 0xF) == 0) {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx12_1_0_rev_0_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx12_1_0_rev_0_hex;
		trap_info->isa_sz =
			sizeof(cwsr_trap_gfx12_1_0_rev_0_hex);
	} else {
		BUILD_BUG_ON(sizeof(cwsr_trap_gfx12_1_0_rev_1_hex) >
			     AMDGPU_TRAP_TBA_MAX_SIZE);
		trap_info->isa_buf = cwsr_trap_gfx12_1_0_rev_1_hex;
		trap_info->isa_sz =
			sizeof(cwsr_trap_gfx12_1_0_rev_1_hex);
	}
}

static uint32_t amdgpu_trap_cwsr_get_vgpr_size_per_cu(struct amdgpu_device *adev)
{
	uint32_t gc_ver = amdgpu_ip_version(adev, GC_HWIP, 0);
	uint32_t vgpr_size;

	switch (gc_ver) {
	case IP_VERSION(9, 4, 1): /* GFX_VERSION_ARCTURUS */
	case IP_VERSION(9, 4, 2): /* GFX_VERSION_ALDEBARAN */
	case IP_VERSION(9, 4, 3): /* GFX_VERSION_AQUA_VANJARAM */
	case IP_VERSION(9, 4, 4): /* GFX_VERSION_AQUA_VANJARAM */
	case IP_VERSION(9, 5, 0):
		vgpr_size = 0x80000;
		break;
	case IP_VERSION(11, 0, 0):
	case IP_VERSION(11, 0, 2):
	case IP_VERSION(11, 0, 3):
	case IP_VERSION(12, 0, 0):
	case IP_VERSION(12, 0, 1):
		vgpr_size = 0x60000;
		break;
	default:
		vgpr_size = 0x40000;
		break;
	}

	return vgpr_size;
}

static uint32_t amdgpu_trap_cwsr_get_wg_ctxt_size_per_cu(struct amdgpu_device *adev)
{
	uint32_t lds_sz_per_cu;

	lds_sz_per_cu =
		(amdgpu_ip_version(adev, GC_HWIP, 0) == IP_VERSION(9, 5, 0)) ?
			(adev->gfx.cu_info.lds_size << 10) :
			LDS_SIZE_PER_CU;

	return amdgpu_trap_cwsr_get_vgpr_size_per_cu(adev) + SGPR_SIZE_PER_CU +
	       lds_sz_per_cu + HWREG_SIZE_PER_CU;
}

static uint32_t amdgpu_trap_cwsr_ctl_stack_bytes_per_wave(struct amdgpu_device *adev)
{
	uint32_t sz;

	if (amdgpu_ip_version(adev, GC_HWIP, 0) >= IP_VERSION(10, 1, 0))
		sz = 12;
	else
		sz = 8;
	return sz;
}

static void amdgpu_trap_cwsr_init_save_area_info(struct amdgpu_device *adev,
						 struct amdgpu_trap_info *trap_info)
{
	struct amdgpu_gfx_config *gfx_info = &adev->gfx.config;
	uint32_t gc_ver = amdgpu_ip_version(adev, GC_HWIP, 0);
	uint32_t ctl_stack_size, wg_data_size, dbg_mem_size;
	uint32_t array_count;
	uint32_t wave_num;
	uint32_t cu_num;

	if (gc_ver < IP_VERSION(9, 0, 1))
		return;

	array_count = gfx_info->max_shader_engines * gfx_info->max_sh_per_se;

	cu_num = adev->gfx.cu_info.number / NUM_XCC(adev->gfx.xcc_mask);
	wave_num = (gc_ver < IP_VERSION(10, 1, 0)) ? /* GFX_VERSION_NAVI10 */
			   min(cu_num * 40,
			       array_count / gfx_info->max_sh_per_se * 512) :
			   cu_num * 32;

	wg_data_size = ALIGN(cu_num * amdgpu_trap_cwsr_get_wg_ctxt_size_per_cu(adev),
			     PAGE_SIZE);
	ctl_stack_size =
		wave_num * amdgpu_trap_cwsr_ctl_stack_bytes_per_wave(adev) + 8;
	ctl_stack_size =
		ALIGN(SIZEOF_HSA_USER_CONTEXT_SAVE_AREA_HEADER + ctl_stack_size,
		      PAGE_SIZE);
	dbg_mem_size =
		ALIGN(wave_num * DEBUGGER_BYTES_PER_WAVE, DEBUGGER_BYTES_ALIGN);
	/*
	* HW design limits control stack size to 0x7000.
	* This is insufficient for theoretical PM4 cases
	* but sufficient for AQL, limited by SPI events.
	*/
	if (IP_VERSION_MAJ(gc_ver) == 10)
		ctl_stack_size = min(ctl_stack_size, 0x7000);

	trap_info->cwsr.xcc_ctl_stack_sz = ctl_stack_size;
	trap_info->cwsr.xcc_cwsr_sz = ctl_stack_size + wg_data_size;
	trap_info->cwsr.xcc_dbg_mem_sz = dbg_mem_size;
}

int amdgpu_trap_init(struct amdgpu_device *adev)
{
	struct amdgpu_trap_info *trap_info __free(kfree) =
		kzalloc(sizeof(*trap_info), GFP_KERNEL);
	void *ptr;
	int r;

	if (!amdgpu_trap_is_supported(adev))
		return -EOPNOTSUPP;

	if (!trap_info)
		return -ENOMEM;
	amdgpu_trap_cwsr_init_isa_details(adev, trap_info);

	if (!trap_info->isa_sz)
		return -EOPNOTSUPP;

	r = amdgpu_bo_create_kernel(adev, AMDGPU_TRAP_TBA_MAX_SIZE, PAGE_SIZE,
				    AMDGPU_GEM_DOMAIN_GTT, &trap_info->isa_bo,
				    NULL, &ptr);
	if (r)
		return r;

	memcpy(ptr, trap_info->isa_buf, trap_info->isa_sz);

	amdgpu_trap_cwsr_init_save_area_info(adev, trap_info);
	adev->trap_info = no_free_ptr(trap_info);

	return 0;
}

void amdgpu_trap_fini(struct amdgpu_device *adev)
{
	if (!amdgpu_trap_is_enabled(adev))
		return;

	amdgpu_bo_free_kernel(&adev->trap_info->isa_bo, NULL, NULL);
	kfree(adev->trap_info);
	adev->trap_info = NULL;
}

/*
 * amdgpu_map_cwsr_trap_handler should be called during amdgpu_vm_init
 * it maps virtual address amdgpu_trap_tba_vaddr() to this VM, and each
 * compute queue can use this virtual address for wave save/restore
 * operations to support compute preemption.
 */
static int amdgpu_trap_map_region(struct amdgpu_device *adev,
				  struct amdgpu_vm *vm,
				  struct amdgpu_trap_obj *cwsr,
				  enum amdgpu_trap_region region)
{
	uint64_t cwsr_addr, va_flags, va;
	struct amdgpu_bo_va **bo_va;
	struct amdgpu_bo *bo;
	uint32_t size;
	int r;

	if (!cwsr || !vm)
		return -EINVAL;

	cwsr_addr = amdgpu_trap_tba_vaddr(adev);

	if (region == AMDGPU_TRAP_TBA) {
		size = AMDGPU_TRAP_TBA_MAX_SIZE;
		bo_va = &cwsr->tba_va;
		bo = adev->trap_info->isa_bo;
		va = cwsr_addr;
		va_flags = (AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE |
			    AMDGPU_VM_PAGE_EXECUTABLE);
	} else {
		size = AMDGPU_TRAP_TMA_MAX_SIZE;
		bo_va = &cwsr->tma_va;
		bo = cwsr->tma_bo;
		va = cwsr_addr + AMDGPU_TRAP_TMA_OFFSET;
		va_flags = (AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE);
	}

	*bo_va = amdgpu_vm_bo_add(adev, vm, bo);
	if (!*bo_va)
		return -ENOMEM;

	va &= AMDGPU_GMC_HOLE_MASK;
	r = amdgpu_vm_bo_map(adev, *bo_va, va, 0, size, va_flags);
	if (r) {
		dev_err(adev->dev, "failed to do bo map of %s region, err=%d\n",
			(region == AMDGPU_TRAP_TBA ? "tba" : "tma"), r);
		amdgpu_vm_bo_del(adev, *bo_va);
		*bo_va = NULL;
		return r;
	}

	r = amdgpu_vm_bo_update(adev, *bo_va, false);
	if (r) {
		dev_err(adev->dev,
			"failed to do page table update of %s region, err=%d\n",
			(region == AMDGPU_TRAP_TBA ? "tba" : "tma"), r);
		amdgpu_vm_bo_del(adev, *bo_va);
		*bo_va = NULL;
		return r;
	}

	if (region == AMDGPU_TRAP_TBA)
		cwsr->tba_gpu_va_addr = va;
	else
		cwsr->tma_gpu_va_addr = va;

	return 0;
}

static int amdgpu_trap_unmap_region(struct amdgpu_device *adev,
				    struct amdgpu_trap_obj *cwsr,
				    enum amdgpu_trap_region region)
{
	struct amdgpu_bo_va **bo_va;
	uint64_t va;
	int r;

	if (!cwsr)
		return -EINVAL;

	if (region == AMDGPU_TRAP_TBA) {
		bo_va = &cwsr->tba_va;
		va = cwsr->tba_gpu_va_addr;
	} else {
		bo_va = &cwsr->tma_va;
		va = cwsr->tma_gpu_va_addr;
	}

	r = amdgpu_vm_bo_unmap(adev, *bo_va, va);
	if (r) {
		dev_err(adev->dev,
			"failed to do bo_unmap on CWSR trap handler, err=%d\n",
			r);
		return r;
	}

	amdgpu_vm_bo_del(adev, *bo_va);
	*bo_va = NULL;

	return r;
}

int amdgpu_trap_alloc(struct amdgpu_device *adev, struct amdgpu_vm *vm,
		      struct amdgpu_trap_obj **trap_obj)
{
	struct amdgpu_bo_param bp;
	struct iosys_map map;
	struct amdgpu_trap_obj *cwsr;
	struct amdgpu_bo *bo;
	struct drm_exec exec;
	int r;

	if (!amdgpu_trap_is_enabled(adev))
		return -EOPNOTSUPP;

	if (!vm || !trap_obj)
		return -EINVAL;

	cwsr = kzalloc(sizeof(*cwsr), GFP_KERNEL);
	if (!cwsr)
		return -ENOMEM;

	bo = adev->trap_info->isa_bo;

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_until_all_locked(&exec) {
		r = amdgpu_vm_lock_pd(vm, &exec, 0);
		drm_exec_retry_on_contention(&exec);
		if (unlikely(r))
			goto err_exec;

		r = drm_exec_lock_obj(&exec, &bo->tbo.base);
		drm_exec_retry_on_contention(&exec);
		if (unlikely(r))
			goto err_exec;
	}

	/*
	 * Create the TMA BO with the VM root reservation. The reservation is
	 * already held by drm_exec, so creating the BO doesn't recursively
	 * acquire another reservation_ww_class_mutex. The TMA remains
	 * unpinned and is covered by the VM root reservation.
	 */
	memset(&bp, 0, sizeof(bp));
	bp.size = AMDGPU_TRAP_TMA_MAX_SIZE;
	bp.byte_align = PAGE_SIZE;
	bp.domain = AMDGPU_GEM_DOMAIN_GTT;
	bp.flags = AMDGPU_GEM_CREATE_CPU_GTT_USWC |
		   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	bp.type = ttm_bo_type_kernel;
	bp.bo_ptr_size = sizeof(struct amdgpu_bo);
	bp.resv = vm->root.bo->tbo.base.resv;

	r = amdgpu_bo_create(adev, &bp, &cwsr->tma_bo);
	if (r)
		goto err_exec;

	/*
	 * The TMA BO is unpinned and movable. Map it only while accessing
	 * its contents so that no CPU mapping can become stale across BO
	 * migration. ttm_bo_vmap() and ttm_bo_vunmap() require the BO
	 * reservation, which is the already held VM root reservation.
	 */
	r = ttm_bo_vmap(&cwsr->tma_bo->tbo, &map);
	if (r)
		goto err_tma;

	/*
	 * Finish initialization with a DMA write barrier before allowing
	 * the GPU to observe the contents.
	 */
	iosys_map_memset(&map, 0, 0, AMDGPU_TRAP_TMA_MAX_SIZE);
	dma_wmb();

	ttm_bo_vunmap(&cwsr->tma_bo->tbo, &map);

	r = amdgpu_trap_map_region(adev, vm, cwsr, AMDGPU_TRAP_TMA);
	if (r)
		goto err_tma;

	r = amdgpu_trap_map_region(adev, vm, cwsr, AMDGPU_TRAP_TBA);
	if (r) {
		amdgpu_trap_unmap_region(adev, cwsr, AMDGPU_TRAP_TMA);
		goto err_tma;
	}

	drm_exec_fini(&exec);

	*trap_obj = cwsr;

	return 0;

err_tma:
	amdgpu_bo_unref(&cwsr->tma_bo);

err_exec:
	drm_exec_fini(&exec);
	kfree(cwsr);
	*trap_obj = NULL;

	return r;
}

static int amdgpu_trap_update_l2_handler(struct amdgpu_trap_obj *trap_obj,
					 u64 tba, u64 tma)
{
	struct iosys_map map;
	int r;

	if (!trap_obj || !trap_obj->tma_bo)
		return -EINVAL;

	dma_resv_assert_held(trap_obj->tma_bo->tbo.base.resv);

	r = ttm_bo_vmap(&trap_obj->tma_bo->tbo, &map);
	if (r)
		return r;

	/*
	 * Publish both descriptor fields before allowing the GPU to observe
	 * the updated second-level trap handler.
	 */
	iosys_map_wr(&map, AMDGPU_TRAP_L2_TBA_INDEX * sizeof(u64),
		     u64, tba);
	iosys_map_wr(&map, AMDGPU_TRAP_L2_TMA_INDEX * sizeof(u64),
		     u64, tma);
	dma_wmb();

	ttm_bo_vunmap(&trap_obj->tma_bo->tbo, &map);

	return 0;
}

static int amdgpu_trap_clear_l2_handler(struct amdgpu_vm *vm,
					struct amdgpu_trap_obj *trap_obj)
{
	if (!trap_obj || !trap_obj->tma_bo)
		return -EINVAL;

	dma_resv_assert_held(vm->root.bo->tbo.base.resv);

	return amdgpu_trap_update_l2_handler(trap_obj, 0, 0);
}

int amdgpu_trap_validate_params(struct amdgpu_device *adev,
				struct amdgpu_trap_params *trap_params,
				int num_xcc)
{
	uint32_t min_size, max_size;

	if (!amdgpu_trap_is_enabled(adev))
		return -EOPNOTSUPP;

	if (!trap_params)
		return -EINVAL;

	min_size = amdgpu_trap_size_needed(adev, num_xcc);
	max_size = 2 * min_size;
	/*
	 * Only save area size details checked. Address validation needs to be
	 * carried out separately. User is expected to pass a cwsr buffer area
	 * with size >= ctl stack size + wg data size + debug memory size
	 */
	if (trap_params->cwsr_sz < min_size ||
	    trap_params->cwsr_sz > max_size) {
		dev_dbg(adev->dev,
			"queue cwsr size 0x%x should be within min %u : max %u\n",
			trap_params->cwsr_sz, min_size, max_size);
		return -EINVAL;
	}

	return 0;
}

void amdgpu_trap_free(struct amdgpu_device *adev, struct amdgpu_vm *vm,
		      struct amdgpu_trap_obj **trap_obj)
{
	struct amdgpu_bo *tba_bo;
	struct drm_exec exec;
	int r;

	if (!trap_obj || !*trap_obj)
		return;

	tba_bo = adev->trap_info->isa_bo;

	drm_exec_init(&exec, 0, 0);
	drm_exec_until_all_locked(&exec) {
		r = amdgpu_vm_lock_pd(vm, &exec, 0);
		drm_exec_retry_on_contention(&exec);
		if (unlikely(r))
			goto out_exec;

		if (tba_bo) {
			r = drm_exec_lock_obj(&exec, &tba_bo->tbo.base);
			drm_exec_retry_on_contention(&exec);
			if (unlikely(r))
				goto out_exec;
		}
	}

	if ((*trap_obj)->tba_va)
		amdgpu_trap_unmap_region(adev,
				*trap_obj,
				AMDGPU_TRAP_TBA);

	if ((*trap_obj)->tma_va)
		amdgpu_trap_unmap_region(adev,
				*trap_obj,
				AMDGPU_TRAP_TMA);

	drm_exec_fini(&exec);
	amdgpu_bo_unref(&(*trap_obj)->tma_bo);
	kfree(*trap_obj);
	*trap_obj = NULL;

	return;

out_exec:
	drm_exec_fini(&exec);
}

static int amdgpu_trap_validate_user_addr(struct amdgpu_device *adev,
					 struct amdgpu_vm *vm,
					 struct amdgpu_trap_usr_addr *usr_addr)
{
	struct amdgpu_bo_va_mapping *va_map;
	u64 start, end, addr, last;

	if (!usr_addr || !usr_addr->size)
		return -EINVAL;

	dma_resv_assert_held(vm->root.bo->tbo.base.resv);

	start = usr_addr->addr & AMDGPU_GMC_HOLE_MASK;

	if (check_add_overflow(start, (u64)usr_addr->size - 1, &end))
		return -EINVAL;

	/*
	 * amdgpu_vm_bo_lookup_mapping() expects a GPU VA,
	 * not a page-shifted address.
	 */
	va_map = amdgpu_vm_bo_lookup_mapping(vm, start);
	if (!va_map)
		return -EINVAL;

	addr = start >> AMDGPU_GPU_PAGE_SHIFT;
	last = end >> AMDGPU_GPU_PAGE_SHIFT;

	if (addr < va_map->start || last > va_map->last)
		return -EINVAL;

	return 0;
}

int amdgpu_trap_vm_set_l2_trap(struct amdgpu_device *adev,
			       struct amdgpu_vm *vm,
			       struct amdgpu_trap_obj *trap_obj,
			       u64 tba_va, uint32_t tba_sz,
			       u64 tma_va, uint32_t tma_sz)
{
	struct amdgpu_trap_usr_addr tba = {
		.addr = tba_va,
		.size = tba_sz,
	};
	struct amdgpu_trap_usr_addr tma = {
		.addr = tma_va,
		.size = tma_sz,
	};
	long timeout;
	int r;

	if (!amdgpu_trap_is_enabled(adev))
		return -EOPNOTSUPP;

	if (!vm || !trap_obj || !trap_obj->tma_bo)
		return -EINVAL;

	r = amdgpu_bo_reserve(vm->root.bo, false);
	if (r)
		return r;

	timeout = dma_resv_wait_timeout(vm->root.bo->tbo.base.resv,
					DMA_RESV_USAGE_BOOKKEEP,
					true, MAX_SCHEDULE_TIMEOUT);
	if (timeout <= 0) {
		r = timeout < 0 ? timeout : -ETIMEDOUT;
		goto out_unreserve;
	}

	r = amdgpu_trap_validate_user_addr(adev, vm, &tba);
	if (r)
		goto out_unreserve;

	r = amdgpu_trap_validate_user_addr(adev, vm, &tma);
	if (r)
		goto out_unreserve;

	r = amdgpu_trap_update_l2_handler(trap_obj, tba_va, tma_va);

out_unreserve:
	amdgpu_bo_unreserve(vm->root.bo);

	return r;
}

int amdgpu_trap_vm_clear_l2_trap(struct amdgpu_device *adev,
				 struct amdgpu_vm *vm,
				 struct amdgpu_trap_obj *trap_obj)
{
	long timeout;
	int r;

	if (!amdgpu_trap_is_enabled(adev))
		return -EOPNOTSUPP;

	if (!vm || !trap_obj || !trap_obj->tma_bo)
		return -EINVAL;

	r = amdgpu_bo_reserve(vm->root.bo, false);
	if (r)
		return r;

	timeout = dma_resv_wait_timeout(vm->root.bo->tbo.base.resv,
					DMA_RESV_USAGE_BOOKKEEP,
					true, MAX_SCHEDULE_TIMEOUT);
	if (timeout <= 0) {
		r = timeout < 0 ? timeout : -ETIMEDOUT;
		goto out_unreserve;
	}

	r = amdgpu_trap_clear_l2_handler(vm, trap_obj);

out_unreserve:
	amdgpu_bo_unreserve(vm->root.bo);

	return r;
}

int amdgpu_trap_set_trap_debug_flag(struct amdgpu_device *adev,
				    struct amdgpu_vm *vm,
				    struct amdgpu_trap_obj *trap_obj,
				    bool enabled)
{
	struct iosys_map map;
	int r;

	if (!amdgpu_trap_is_enabled(adev))
		return -EOPNOTSUPP;

	if (!vm || !trap_obj || !trap_obj->tma_bo)
		return -EINVAL;

	r = amdgpu_bo_reserve(vm->root.bo, false);
	if (r)
		return r;

	r = ttm_bo_vmap(&trap_obj->tma_bo->tbo, &map);
	if (r)
		goto out_unreserve;

	iosys_map_wr(&map, AMDGPU_TRAP_DEBUG_FLAG_INDEX * sizeof(u64),
		     u64, (u64)enabled);
	dma_wmb();

	ttm_bo_vunmap(&trap_obj->tma_bo->tbo, &map);

out_unreserve:
	amdgpu_bo_unreserve(vm->root.bo);

	return r;
}
