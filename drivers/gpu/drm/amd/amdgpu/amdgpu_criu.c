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
#include <linux/interval_tree_generic.h>

#include "amdgpu.h"
#include "amdgpu_display.h"
#include "amdgpu_dma_buf.h"
#include "amdgpu_hmm.h"
#include "amdgpu_xgmi.h"

static uint32_t hardware_flags_to_uapi_flags(struct amdgpu_device *adev, uint64_t pte_flags)
{
	uint32_t gem_flags = 0;

	//This function will be replaced by the mapping flags rework

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


/**
 * amdgpu_criu_bo_info_ioctl - get information about a process' buffer objects
 *
 * @dev: drm device pointer
 * @data: drm_amdgpu_criu_bo_info_args
 * @filp: drm file pointer
 *
 * num_bos is set as an input to the size of the bo_buckets array.
 * num_bos is sent back as output as the number of bos in the process.
 * If that number is larger than the size of the array, the ioctl must
 * be retried.
 *
 * Returns:
 * 0 for success, -errno for errors.
 */
int amdgpu_criu_bo_info_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *filp)
{
	struct drm_amdgpu_criu_bo_info_args *args = data;
	struct drm_amdgpu_criu_bo_bucket *bo_buckets;
	struct drm_gem_object *gobj;
	int id, ret = 0;
	int bo_index = 0;
	int num_bos = 0;

	spin_lock(&filp->table_lock);
	idr_for_each_entry(&filp->object_idr, gobj, id)
		num_bos += 1;
	spin_unlock(&filp->table_lock);

	if (args->num_bos < num_bos) {
		args->num_bos = num_bos;
		goto exit;
	}
	args->num_bos = num_bos;
	if (num_bos == 0) {
		goto exit;
	}

	bo_buckets = kvzalloc(num_bos * sizeof(*bo_buckets), GFP_KERNEL);
	if (!bo_buckets) {
		ret = -ENOMEM;
		goto free_buckets;
	}

	spin_lock(&filp->table_lock);
	idr_for_each_entry(&filp->object_idr, gobj, id) {
		struct amdgpu_bo *bo = gem_to_amdgpu_bo(gobj);
		struct drm_amdgpu_criu_bo_bucket *bo_bucket;

		bo_bucket = &bo_buckets[bo_index];

		bo_bucket->size = amdgpu_bo_size(bo);
		bo_bucket->alloc_flags = bo->flags & (~AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE);
		bo_bucket->preferred_domains = bo->preferred_domains;
		bo_bucket->gem_handle = id;

		if (bo->tbo.base.import_attach)
			bo_bucket->flags |= AMDGPU_CRIU_BO_FLAG_IS_IMPORT;

		bo_index += 1;
	}
	spin_unlock(&filp->table_lock);

	ret = copy_to_user((void __user *)args->bo_buckets, bo_buckets, num_bos * sizeof(*bo_buckets));
	if (ret) {
		pr_debug("Failed to copy BO information to user\n");
		ret = -EFAULT;
	}

free_buckets:
	kvfree(bo_buckets);
exit:

	return ret;
}
