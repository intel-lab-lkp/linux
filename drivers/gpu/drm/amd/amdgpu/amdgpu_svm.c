/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
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
 *
 */

#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include <drm/drm_file.h>

#include "amdgpu.h"
#include "amdgpu_svm.h"
#include "amdgpu_svm_attr.h"
#include "amdgpu_svm_range.h"
#include "amdgpu_vm.h"

#if IS_ENABLED(CONFIG_DRM_AMDGPU_SVM)

#define AMDGPU_SVM_MAX_ATTRS 64
#define AMDGPU_SVM_DEFAULT_SVM_NOTIFIER_SIZE 512

static const unsigned long amdgpu_svm_chunk_sizes[] = {
	SZ_2M,
	SZ_64K,
	SZ_4K,
};

static struct kmem_cache *amdgpu_svm_range_cache;

static void amdgpu_svm_invalidate(struct drm_gpusvm *gpusvm,
				  struct drm_gpusvm_notifier *notifier,
				  const struct mmu_notifier_range *mmu_range)
{
	amdgpu_svm_range_invalidate(to_amdgpu_svm(gpusvm), notifier, mmu_range);
}

static struct drm_gpusvm_range *amdgpu_svm_range_alloc(struct drm_gpusvm *gpusvm)
{
	struct amdgpu_svm_range *range;

	range = kmem_cache_zalloc(amdgpu_svm_range_cache, GFP_KERNEL);
	if (!range)
		return NULL;

	INIT_LIST_HEAD(&range->gc_node);
	range->pending_start = ULONG_MAX;
	return &range->base;
}

static void amdgpu_svm_range_free(struct drm_gpusvm_range *range)
{
	kmem_cache_free(amdgpu_svm_range_cache, to_amdgpu_svm_range(range));
}

static const struct drm_gpusvm_ops amdgpu_gpusvm_ops = {
	.range_alloc = amdgpu_svm_range_alloc,
	.range_free = amdgpu_svm_range_free,
	.invalidate = amdgpu_svm_invalidate,
};

static void amdgpu_svm_release(struct kref *ref)
{
	kfree(container_of(ref, struct amdgpu_svm, refcount));
}

static void amdgpu_svm_put(struct amdgpu_svm *svm)
{
	if (svm)
		kref_put(&svm->refcount, amdgpu_svm_release);
}

static struct amdgpu_svm *
amdgpu_svm_lookup_by_pasid(struct amdgpu_device *adev, uint32_t pasid)
{
	struct amdgpu_svm *svm = NULL;
	struct amdgpu_vm *vm;
	unsigned long irqflags;

	xa_lock_irqsave(&adev->vm_manager.pasids, irqflags);
	vm = xa_load(&adev->vm_manager.pasids, pasid);
	if (vm && vm->svm) {
		svm = vm->svm;
		kref_get(&svm->refcount);
	}
	xa_unlock_irqrestore(&adev->vm_manager.pasids, irqflags);

	return svm;
}

int amdgpu_svm_cache_init(void)
{
	int ret = 0;

	if (amdgpu_svm_range_cache)
		return 0;

	amdgpu_svm_range_cache = AMDGPU_SVM_KMEM_CACHE_CREATE("amdgpu_svm_range_cache",
								 struct amdgpu_svm_range);
	if (!amdgpu_svm_range_cache)
		return -ENOMEM;

	ret = amdgpu_svm_attr_cache_init();
	if (ret)
		goto free_out;

	return 0;
free_out:
	amdgpu_svm_attr_cache_fini();
	AMDGPU_SVM_KMEM_CACHE_DESTROY(amdgpu_svm_range_cache);
	return ret;
}

void amdgpu_svm_cache_fini(void)
{
	if (!amdgpu_svm_range_cache)
		return;

	amdgpu_svm_attr_cache_fini();
	AMDGPU_SVM_KMEM_CACHE_DESTROY(amdgpu_svm_range_cache);
}

static int amdgpu_svm_set_attr(struct amdgpu_vm *vm,
			      uint64_t start,
			      uint64_t size,
			      uint32_t nattr,
			      const struct drm_amdgpu_svm_attribute *attrs)
{
	struct amdgpu_svm *svm = vm->svm;

	/* cause drm_gpusvm_range_find_or_insert acquire the mmap_read lock
	 * can not acquire the mmap lock in the entire time in ioctl
	 * just flush the work to  reduce the probability of failure
	 */
	amdgpu_svm_range_sync_work(svm);

	return amdgpu_svm_attr_set(svm->attr_tree, start, size, nattr,
				   attrs);
}

static int amdgpu_svm_get_attr(struct amdgpu_vm *vm,
			      uint64_t start,
			      uint64_t size,
			      uint32_t nattr,
			      struct drm_amdgpu_svm_attribute *attrs)
{
	return amdgpu_svm_attr_get(vm->svm->attr_tree, start, size, nattr, attrs);
}

static bool amdgpu_svm_default_xnack_enabled(struct amdgpu_device *adev)
{
	uint32_t gc_ver = amdgpu_ip_version(adev, GC_HWIP, 0);

	if (gc_ver < IP_VERSION(9, 0, 1))
		return false;
	if (!amdgpu_sriov_xnack_support(adev))
		return false;

	switch (gc_ver) {
	case IP_VERSION(9, 4, 2):
	case IP_VERSION(9, 4, 3):
	case IP_VERSION(9, 4, 4):
	case IP_VERSION(9, 5, 0):
		return true;
	default:
		break;
	}
	if (gc_ver >= IP_VERSION(10, 1, 1))
		return false;
	return !adev->gmc.noretry;
}

static void amdgpu_svm_flush_tlb_compute(struct amdgpu_svm *svm)
{
	amdgpu_vm_flush_compute_tlb(svm->adev, svm->vm, TLB_FLUSH_HEAVYWEIGHT,
				    svm->adev->gfx.xcc_mask);
}

static int amdgpu_svm_init_with_ops(struct amdgpu_device *adev,
				    struct amdgpu_vm *vm,
				    void (*begin_restore)(struct amdgpu_svm *),
				    void (*end_restore)(struct amdgpu_svm *),
				    void (*flush_tlb)(struct amdgpu_svm *))
{
	struct amdgpu_svm *svm;
	int ret;

	if (vm->svm)
		return 0;

	ret = amdgpu_svm_cache_init();
	if (ret)
		return ret;

	svm = kzalloc(sizeof(*svm), GFP_KERNEL);
	if (!svm)
		return -ENOMEM;

	kref_init(&svm->refcount);
	svm->adev = adev;
	svm->vm = vm;

	svm->default_granularity = min_t(u8, amdgpu_svm_default_granularity, 0x3f);
	svm->xnack_enabled = amdgpu_svm_default_xnack_enabled(adev);
	svm->xnack_enabled = false; // WA/POC: force to disable xnack
	svm->begin_restore = begin_restore;
	svm->end_restore = end_restore;
	svm->flush_tlb = flush_tlb;
	atomic_set(&svm->kfd_queues_quiesced, 0);
	atomic_set(&svm->evicted_ranges, 0);
	atomic_set(&svm->exiting, 0);

	ret = amdgpu_svm_range_work_init(svm);
	if (ret)
		goto err_free;

	svm->attr_tree = amdgpu_svm_attr_tree_create(svm);
	if (!svm->attr_tree) {
		ret = -ENOMEM;
		goto err_range_work_fini;
	}

	ret = drm_gpusvm_init(&svm->gpusvm, "AMDGPU SVM",
						adev_to_drm(adev), current->mm, 0,
						adev->vm_manager.max_pfn << AMDGPU_GPU_PAGE_SHIFT,
						AMDGPU_SVM_DEFAULT_SVM_NOTIFIER_SIZE * SZ_1M,
						&amdgpu_gpusvm_ops,
						amdgpu_svm_chunk_sizes,
						ARRAY_SIZE(amdgpu_svm_chunk_sizes));

	if (ret)
		goto err_attr_tree_destroy;

	drm_gpusvm_driver_set_lock(&svm->gpusvm, &svm->svm_lock);
	vm->svm = svm;
	return 0;

err_attr_tree_destroy:
	amdgpu_svm_attr_tree_destroy(svm->attr_tree);
err_range_work_fini:
	amdgpu_svm_range_work_fini(svm);
err_free:
	kfree(svm);
	return ret;
}

static int amdgpu_svm_init_compute(struct amdgpu_device *adev, struct amdgpu_vm *vm)
{
	return amdgpu_svm_init_with_ops(adev, vm,
					amdgpu_svm_range_restore_begin_compute,
					amdgpu_svm_range_restore_end_compute,
					amdgpu_svm_flush_tlb_compute);
}

int amdgpu_svm_init(struct amdgpu_device *adev, struct amdgpu_vm *vm)
{
	/* graphics svm init maybe different */

	return amdgpu_svm_init_compute(adev, vm);
}

void amdgpu_svm_close(struct amdgpu_vm *vm)
{
	if (!vm->svm)
		return;

	if (atomic_xchg(&vm->svm->exiting, 1))
		return;

	amdgpu_svm_range_sync_work(vm->svm);
}

void amdgpu_svm_fini(struct amdgpu_vm *vm)
{
	struct amdgpu_svm *svm = vm->svm;

	if (!svm)
		return;

	amdgpu_svm_close(vm);
	down_write(&svm->svm_lock);
	drm_gpusvm_fini(&svm->gpusvm);
	up_write(&svm->svm_lock);

	amdgpu_svm_range_work_fini(svm);
	amdgpu_svm_attr_tree_destroy(svm->attr_tree);
	vm->svm = NULL;
	amdgpu_svm_put(svm);
}

int amdgpu_svm_handle_fault(struct amdgpu_device *adev, uint32_t pasid,
			    uint64_t fault_addr, bool write_fault)
{
	struct amdgpu_svm *svm;
	unsigned long fault_page;
	int ret;

	AMDGPU_SVM_TRACE("handle_fault enter: pasid=%u addr=0x%llx write=%d\n",
			 pasid, fault_addr, write_fault ? 1 : 0);

	svm = amdgpu_svm_lookup_by_pasid(adev, pasid);
	if (!svm) {
		AMDGPU_SVM_TRACE("handle_fault: pasid %u lookup failed\n", pasid);
		return -EOPNOTSUPP;
	}

	AMDGPU_SVM_TRACE("handle_fault: pasid %u svm=%p exiting=%d xnack=%d\n",
			 pasid, svm, atomic_read(&svm->exiting),
			 svm->xnack_enabled ? 1 : 0);

	if (atomic_read(&svm->exiting)) {
		ret = -EAGAIN;
		goto out;
	}

	if (!svm->xnack_enabled) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	fault_page = fault_addr >> PAGE_SHIFT;
	AMDGPU_SVM_TRACE("handle_fault: map_attr page=0x%lx\n", fault_page);

	down_write(&svm->svm_lock);
	ret = amdgpu_svm_range_map_attr_ranges(svm, fault_page, fault_page);
	up_write(&svm->svm_lock);

	if (ret)
		AMDGPU_SVM_TRACE("fault map failed: ret=%d addr=0x%llx write=%d\n",
				 ret, fault_addr, write_fault ? 1 : 0);
	else
		AMDGPU_SVM_TRACE("fault map success: addr=0x%llx write=%d\n",
				 fault_addr, write_fault ? 1 : 0);

out:
	AMDGPU_SVM_TRACE("handle_fault exit: pasid=%u addr=0x%llx ret=%d\n",
			 pasid, fault_addr, ret);
	amdgpu_svm_put(svm);
	return ret;
}

bool amdgpu_svm_is_enabled(struct amdgpu_vm *vm)
{
	return vm->svm != NULL;
}

static int amdgpu_svm_copy_attrs(const struct drm_amdgpu_gem_svm *args,
					   struct drm_amdgpu_svm_attribute **attrs,
					   size_t *size)
{
	if (!args->nattr || args->nattr > AMDGPU_SVM_MAX_ATTRS)
		return -EINVAL;
	if (!args->attrs_ptr)
		return -EINVAL;

	*size = args->nattr * sizeof(**attrs);
	*attrs = memdup_user(u64_to_user_ptr(args->attrs_ptr), *size);

	return PTR_ERR_OR_ZERO(*attrs);
}

int amdgpu_gem_svm_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *filp)
{
	struct amdgpu_fpriv *fpriv = filp->driver_priv;
	struct drm_amdgpu_gem_svm *args = data;
	struct drm_amdgpu_svm_attribute *attrs = NULL;
	struct amdgpu_vm *vm;
	size_t attrs_size = 0;
	int ret = 0;

	AMDGPU_SVM_TRACE("ioctl op=%u va:[0x%llx-0x%llx)-0x%llx nattr=%u\n",
			 args->operation, args->start_addr, args->start_addr + args->size,
			 args->size, args->nattr);

	vm = &fpriv->vm;
	if (!amdgpu_svm_is_enabled(vm))
		return -EOPNOTSUPP;

	if ((args->start_addr & ~PAGE_MASK) || (args->size & ~PAGE_MASK))
		return -EINVAL;

	if (!args->start_addr || !args->size)
		return -EINVAL;

	ret = amdgpu_svm_copy_attrs(args, &attrs, &attrs_size);
	if (ret)
		return ret;

	switch (args->operation) {
	case AMDGPU_SVM_OP_SET_ATTR:
		ret = amdgpu_svm_set_attr(vm, args->start_addr, args->size,
					 args->nattr, attrs);
		break;
	case AMDGPU_SVM_OP_GET_ATTR:
		ret = amdgpu_svm_get_attr(vm, args->start_addr, args->size,
					 args->nattr, attrs);
		if (!ret && copy_to_user(u64_to_user_ptr(args->attrs_ptr),
					 attrs, attrs_size))
			ret = -EFAULT;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	kvfree(attrs);
	return ret;
}

#endif /* CONFIG_DRM_AMDGPU_SVM */
