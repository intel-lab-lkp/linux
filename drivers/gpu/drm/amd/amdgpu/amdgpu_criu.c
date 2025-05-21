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

#include <linux/dma-buf.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/random.h>

#include <drm/amdgpu_drm.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>

#include "amdgpu_criu.h"

#include <drm/amdgpu_drm.h>
#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/ttm/ttm_tt.h>

#include "amdgpu.h"
#include "amdgpu_display.h"
#include "amdgpu_dma_buf.h"
#include "amdgpu_hmm.h"
#include "amdgpu_xgmi.h"

static bool is_import(struct amdgpu_bo *bo)
{
	if (bo->tbo.base.import_attach)
		return &bo->tbo.base != (struct drm_gem_object *)bo->tbo.base.import_attach->dmabuf->priv;
	return false;
}

static int amdgpu_criu_process_info(struct drm_device *dev, struct drm_file *data,
			    struct drm_amdgpu_criu_args *args)
{
	struct drm_gem_object *gobj;
	int id;
	int num_bos = 0;
	int num_vm_mappings = 0;
	struct amdgpu_vm *avm = &((struct amdgpu_fpriv *)data->driver_priv)->vm;

	spin_lock(&data->table_lock);
	idr_for_each_entry(&data->object_idr, gobj, id) {
		struct amdgpu_bo *bo = gem_to_amdgpu_bo(gobj);
		struct amdgpu_vm_bo_base *vm_bo_base;

		num_bos += 1;

		vm_bo_base = bo->vm_bo;

		while (vm_bo_base) {
			struct amdgpu_bo_va *bo_va = container_of(vm_bo_base, struct amdgpu_bo_va, base);
			struct amdgpu_bo_va_mapping *mapping;

			if (vm_bo_base->vm == avm) {

				list_for_each_entry(mapping, &bo_va->invalids, list) {
					num_vm_mappings += 1;
				}
				list_for_each_entry(mapping, &bo_va->valids, list) {
					num_vm_mappings += 1;
				}
			}

			vm_bo_base = vm_bo_base->next;
		}
	}
	spin_unlock(&data->table_lock);

	args->num_bos = num_bos;
	args->num_vms = num_vm_mappings;
	args->pid = avm->task_info->pid;

	return 0;
}

static uint32_t hardware_flags_to_uapi_flags(struct amdgpu_device *adev, uint64_t pte_flags)
{
	uint32_t gem_flags = 0;

	if (pte_flags & AMDGPU_PTE_EXECUTABLE)
		gem_flags |= AMDGPU_VM_PAGE_EXECUTABLE;
	if (pte_flags & AMDGPU_PTE_READABLE)
		gem_flags |= AMDGPU_VM_PAGE_READABLE;
	if (pte_flags & AMDGPU_PTE_WRITEABLE)
		gem_flags |= AMDGPU_VM_PAGE_WRITEABLE;
	if (pte_flags & AMDGPU_PTE_PRT_FLAG(adev))
		gem_flags |= AMDGPU_VM_PAGE_PRT;
	if (pte_flags & AMDGPU_PTE_NOALLOC)
		gem_flags |= AMDGPU_VM_PAGE_NOALLOC;

	return gem_flags;
}

static int amdgpu_criu_checkpoint(struct drm_device *dev, struct drm_file *data,
			    struct drm_amdgpu_criu_args *args)
{

	struct amdgpu_vm *avm = &((struct amdgpu_fpriv *)data->driver_priv)->vm;
	struct drm_amdgpu_criu_bo_bucket *bo_buckets;
	struct drm_amdgpu_criu_vm_bucket *vm_buckets;
	struct drm_gem_object *gobj;
	int vm_priv_index = 0;
	int bo_index = 0;
	int num_bos = 0;
	int fd, id, ret;

	spin_lock(&data->table_lock);
	idr_for_each_entry(&data->object_idr, gobj, id)
		num_bos += 1;
	spin_unlock(&data->table_lock);

	if (args->num_bos != num_bos) {
		ret = -EINVAL;
		goto exit;
	}

	bo_buckets = kvzalloc(num_bos * sizeof(*bo_buckets), GFP_KERNEL);
	if (!bo_buckets) {
		ret = -ENOMEM;
		goto free_buckets;
	}

	vm_buckets = kvzalloc(args->num_vms * sizeof(*vm_buckets), GFP_KERNEL);
	if (!vm_buckets) {
		ret = -ENOMEM;
		goto free_vms;
	}

	idr_for_each_entry(&data->object_idr, gobj, id) {
		struct amdgpu_bo *bo = gem_to_amdgpu_bo(gobj);
		struct drm_amdgpu_criu_bo_bucket *bo_bucket;
		struct amdgpu_vm_bo_base *vm_bo_base;

		bo_bucket = &bo_buckets[bo_index];

		bo_bucket->size = amdgpu_bo_size(bo);
		bo_bucket->offset = amdgpu_bo_mmap_offset(bo);
		bo_bucket->alloc_flags = bo->flags & (!AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE);
		bo_bucket->preferred_domains = bo->preferred_domains;

		if (is_import(bo))
			bo_bucket->flags |= AMDGPU_CRIU_BO_FLAG_IS_IMPORT;

		drm_gem_prime_handle_to_fd(dev, data, id, 0, &fd);
		if (fd)
			bo_bucket->dmabuf_fd = fd;

		vm_bo_base = bo->vm_bo;

		while (vm_bo_base) {
			struct amdgpu_bo_va *bo_va = container_of(vm_bo_base, struct amdgpu_bo_va, base);
			struct amdgpu_bo_va_mapping *mapping;

			if (vm_bo_base->vm == avm) {
				list_for_each_entry(mapping, &bo_va->invalids, list) {
					vm_buckets[vm_priv_index].start = mapping->start;
					vm_buckets[vm_priv_index].last = mapping->last;
					vm_buckets[vm_priv_index].offset = mapping->offset;
					vm_buckets[vm_priv_index].flags = hardware_flags_to_uapi_flags(drm_to_adev(dev), mapping->flags);
					vm_buckets[vm_priv_index].gem_handle = id;
					vm_priv_index += 1;

					bo_bucket->addr = mapping->start * AMDGPU_GPU_PAGE_SIZE;
				}
				list_for_each_entry(mapping, &bo_va->valids, list) {
					vm_buckets[vm_priv_index].start = mapping->start;
					vm_buckets[vm_priv_index].last = mapping->last;
					vm_buckets[vm_priv_index].offset = mapping->offset;
					vm_buckets[vm_priv_index].flags = hardware_flags_to_uapi_flags(drm_to_adev(dev), mapping->flags);
					vm_buckets[vm_priv_index].gem_handle = id;
					vm_priv_index += 1;

					bo_bucket->addr = mapping->start * AMDGPU_GPU_PAGE_SIZE;
				}
			}

			vm_bo_base = vm_bo_base->next;
		}

		bo_index += 1;
	}

	ret = copy_to_user((void __user *)args->bos, bo_buckets, num_bos * sizeof(*bo_buckets));
	if (ret) {
		pr_debug("Failed to copy BO information to user\n");
		ret = -EFAULT;
		goto free_vms;
	}

	ret = copy_to_user((void __user *)args->vms, vm_buckets, args->num_vms * sizeof(*vm_buckets));
	if (ret) {
		pr_debug("Failed to copy BO information to user\n");
		ret = -EFAULT;
		goto free_vms;
	}

free_vms:
	kvfree(vm_buckets);
free_buckets:
	kvfree(bo_buckets);
exit:

	return ret;
}

int amdgpu_criu_op_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *filp)
{
	struct drm_amdgpu_criu_args *args = data;
	int ret;

	switch (args->op) {
	case AMDGPU_CRIU_OP_PROCESS_INFO:
		ret = amdgpu_criu_process_info(dev, filp, args);
		break;
	case AMDGPU_CRIU_OP_CHECKPOINT:
		ret = amdgpu_criu_checkpoint(dev, filp, args);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
