// SPDX-License-Identifier: GPL-2.0 OR MIT
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
#include "amdgpu_svm_fault.h"
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

#define AMDGPU_SVM_GC_WQ_NAME "amdgpu_svm_gc"
#define XNACK_OFF(svm)	((svm)->xnack_enabled == false)
#define XNACK_ON(svm)	((svm)->xnack_enabled == true)

static struct kmem_cache *amdgpu_svm_range_cache;
static DEFINE_MUTEX(amdgpu_svm_cache_lock);

static void amdgpu_svm_invalidate(struct drm_gpusvm *gpusvm,
				  struct drm_gpusvm_notifier *notifier,
				  const struct mmu_notifier_range *mmu_range)
{
	struct amdgpu_svm *svm = to_amdgpu_svm(gpusvm);
	struct drm_gpusvm_range *first;
	uint64_t adj_start = mmu_range->start, adj_end = mmu_range->end;

	amdgpu_svm_assert_in_notifier(svm);

	AMDGPU_SVM_TRACE(
		"INVALIDATE: pasid=%u, gpusvm=%p, seqno=%lu, [0x%016lx-0x%016lx]-0x%lx, ev=%d\n",
		svm->vm->pasid, &svm->gpusvm,
		notifier->notifier.invalidate_seq,
			 mmu_range->start, mmu_range->end,
			 mmu_range->end - mmu_range->start, mmu_range->event);

	if (mmu_range->event == MMU_NOTIFY_RELEASE)
		return;
	if (atomic_read(&svm->exiting))
		return;

	adj_start = max(drm_gpusvm_notifier_start(notifier), adj_start);
	adj_end = min(drm_gpusvm_notifier_end(notifier), adj_end);

	first = drm_gpusvm_range_find(notifier, adj_start, adj_end);
	if (!first)
		return;

	svm->invalidate_ranges(svm, notifier, mmu_range, first,
			       adj_start, adj_end);
}

static struct drm_gpusvm_range *amdgpu_svm_range_alloc(struct drm_gpusvm *gpusvm)
{
	struct amdgpu_svm_range *range;

	range = kmem_cache_zalloc(amdgpu_svm_range_cache, GFP_KERNEL);
	if (!range)
		return NULL;

	INIT_LIST_HEAD(&range->work_node);
	range->pending_start_page = ULONG_MAX;
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

void amdgpu_svm_put(struct amdgpu_svm *svm)
{
	if (svm)
		kref_put(&svm->refcount, amdgpu_svm_release);
}

struct amdgpu_svm *
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

	mutex_lock(&amdgpu_svm_cache_lock);

	if (amdgpu_svm_range_cache) {
		mutex_unlock(&amdgpu_svm_cache_lock);
		return 0;
	}

	amdgpu_svm_range_cache = AMDGPU_SVM_KMEM_CACHE_CREATE("amdgpu_svm_range_cache",
								 struct amdgpu_svm_range);
	if (!amdgpu_svm_range_cache) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	ret = amdgpu_svm_attr_cache_init();
	if (ret)
		goto free_out;

	mutex_unlock(&amdgpu_svm_cache_lock);
	return 0;

free_out:
	amdgpu_svm_attr_cache_fini();
	AMDGPU_SVM_KMEM_CACHE_DESTROY(amdgpu_svm_range_cache);
out_unlock:
	mutex_unlock(&amdgpu_svm_cache_lock);
	return ret;
}

void amdgpu_svm_cache_fini(void)
{
	if (!amdgpu_svm_range_cache)
		return;

	amdgpu_svm_attr_cache_fini();
	AMDGPU_SVM_KMEM_CACHE_DESTROY(amdgpu_svm_range_cache);
}

static int amdgpu_svm_op_set_attr(struct amdgpu_vm *vm,
				  uint64_t start,
				  uint64_t size,
				  uint32_t nattr,
				  const struct drm_amdgpu_svm_attribute *attrs)
{
	struct amdgpu_svm *svm = vm->svm;

	amdgpu_svm_sync_work(svm);

	return amdgpu_svm_attr_set(svm->attr_tree, start, size, nattr,
				   attrs);
}

static int amdgpu_svm_op_get_attr(struct amdgpu_vm *vm,
				  uint64_t start,
				  uint64_t size,
				  uint32_t nattr,
				  struct drm_amdgpu_svm_attribute *attrs)
{
	amdgpu_svm_sync_work(vm->svm);

	return amdgpu_svm_attr_get(vm->svm->attr_tree, start, size, nattr, attrs);
}

static int amdgpu_svm_op_reset_attr(struct amdgpu_vm *vm,
				    uint64_t start, uint64_t size)
{
	struct amdgpu_svm *svm = vm->svm;
	unsigned long start_page = start >> PAGE_SHIFT;
	unsigned long last_page = (start + size - 1) >> PAGE_SHIFT;

	amdgpu_svm_sync_work(svm);

	return amdgpu_svm_attr_reset(svm->attr_tree,
				     start_page, last_page);
}

static uint32_t
attr_change_trigger(const struct amdgpu_svm_attrs *old_attrs,
		    const struct amdgpu_svm_attrs *new_attrs)
{
	uint32_t trigger = 0;
	uint32_t changed_flags = old_attrs->flags ^ new_attrs->flags;

	if (old_attrs->access != new_attrs->access)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_ACCESS_CHANGE;
	if (changed_flags & AMDGPU_SVM_PTE_FLAG_MASK)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_PTE_FLAG_CHANGE;
	if (changed_flags & AMDGPU_SVM_MAPPING_FLAG_MASK)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_MAPPING_FLAG_CHANGE;
	if (old_attrs->preferred_loc != new_attrs->preferred_loc ||
	    old_attrs->prefetch_loc != new_attrs->prefetch_loc)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_LOCATION_CHANGE;
	if (old_attrs->granularity != new_attrs->granularity)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_GRANULARITY_CHANGE;
	if (new_attrs->prefetch_loc != AMDGPU_SVM_LOCATION_UNDEFINED &&
	    new_attrs->prefetch_loc != AMDGPU_SVM_LOCATION_SYSMEM)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_PREFETCH;

	return trigger;
}

int amdgpu_svm_apply_attr_change(struct amdgpu_svm *svm,
				 const struct amdgpu_svm_attrs *old_attrs,
				 const struct amdgpu_svm_attrs *new_attrs,
				 unsigned long start_page,
				 unsigned long last_page)
{
	bool old_access, new_access;
	bool needs_invalidate = false;
	bool needs_mapping = false;
	uint32_t trigger;
	int ret;

	amdgpu_svm_assert_locked(svm);

	if (!start_page && !last_page)
		return 0;

	trigger = attr_change_trigger(old_attrs, new_attrs);
	old_access = amdgpu_svm_attr_has_access(old_attrs->access);
	new_access = amdgpu_svm_attr_has_access(new_attrs->access);
	if (XNACK_ON(svm) &&
	    (trigger & AMDGPU_SVM_ATTR_TRIGGER_NEED_INVALIDATE))
		needs_invalidate = true;

	if (trigger & AMDGPU_SVM_ATTR_TRIGGER_PREFETCH)
		needs_mapping = true;

	if (!trigger && !needs_mapping)
		return 0;

	AMDGPU_SVM_TRACE("attr change trigger=0x%x old=%d new=%d [0x%lx-0x%lx]-0x%lx, xnack=%d\n",
			 trigger, old_access, new_access, start_page, last_page,
			 last_page - start_page + 1,
			 svm->xnack_enabled ? 1 : 0);

	if (needs_invalidate) {
		AMDGPU_SVM_TRACE("attr change invalidate [0x%lx-0x%lx]-0x%lx trigger=0x%x\n",
				 start_page, last_page,
				 last_page - start_page + 1, trigger);
		ret = amdgpu_svm_range_invalidate_interval(svm, start_page,
							   last_page);
		if (ret) {
			AMDGPU_SVM_ERR(
				"failed to invalidate range for attr change: [0x%lx-0x%lx], ret=%d\n",
				start_page, last_page, ret);
			return ret;
		}
	}

	if (!needs_mapping)
		return 0;

	return amdgpu_svm_range_map_attrs(svm, new_attrs,
					  start_page << PAGE_SHIFT,
					  (last_page + 1) << PAGE_SHIFT);
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

static int amdgpu_svm_work_init(struct amdgpu_svm *svm,
				void (*gc_work_func)(struct work_struct *));
static void amdgpu_svm_work_fini(struct amdgpu_svm *svm);

static int amdgpu_svm_init_xnack_mode(struct amdgpu_device *adev,
				    enum amdgpu_svm_xnack_mode mode,
				    bool *xnack_enabled)
{
	bool xnack_default = amdgpu_svm_default_xnack_enabled(adev);

	switch (mode) {
	case AMDGPU_SVM_XNACK_DEFAULT:
		*xnack_enabled = xnack_default;
		break;
	case AMDGPU_SVM_XNACK_ON:
		if (!xnack_default) {
			AMDGPU_SVM_ERR("xnack on not available (mode=%d)\n",
					mode);
			*xnack_enabled = xnack_default;
			return -EOPNOTSUPP;
		}
		*xnack_enabled = true;
		break;
	case AMDGPU_SVM_XNACK_OFF:
		*xnack_enabled = false;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int amdgpu_svm_init_with_ops(struct amdgpu_svm *svm,
				    void (*invalidate_ranges)(struct amdgpu_svm *,
						struct drm_gpusvm_notifier *,
						const struct mmu_notifier_range *,
						struct drm_gpusvm_range *,
						uint64_t, uint64_t),
				    void (*gc_work_func)(struct work_struct *),
				    void (*flush_tlb)(struct amdgpu_svm *))
{
	struct amdgpu_device *adev = svm->adev;
	int ret;

	svm->flush_tlb = flush_tlb;
	svm->invalidate_ranges = invalidate_ranges;

	ret = amdgpu_svm_work_init(svm, gc_work_func);
	if (ret)
		return ret;

	svm->attr_tree = amdgpu_svm_attr_tree_create(svm);
	if (!svm->attr_tree) {
		ret = -ENOMEM;
		goto err_work_fini;
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

	return 0;

err_attr_tree_destroy:
	amdgpu_svm_attr_tree_destroy(svm->attr_tree);
err_work_fini:
	amdgpu_svm_work_fini(svm);
	return ret;
}

static void amdgpu_svm_gc_work_func(struct work_struct *w);

static int amdgpu_svm_init_compute(struct amdgpu_device *adev,
				   struct amdgpu_vm *vm,
				   enum amdgpu_svm_xnack_mode xnack_mode)
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
	svm->default_granularity = min_t(u8, amdgpu_svm_default_granularity, 0x1B);
	atomic_set(&svm->exiting, 0);

	ret = amdgpu_svm_init_xnack_mode(adev, xnack_mode,
					  &svm->xnack_enabled);
	if (ret)
		goto err_free;

	if (svm->xnack_enabled) {
		ret = amdgpu_svm_init_with_ops(svm,
					       amdgpu_svm_range_invalidate,
					       amdgpu_svm_gc_work_func,
					       amdgpu_svm_flush_tlb_compute);
	} else {
		AMDGPU_SVM_ERR("xnack off is not supported yet\n");
		ret = -EOPNOTSUPP;
	}

	if (ret)
		goto err_free;

	AMDGPU_SVM_TRACE("AMDGPU SVM initialized: default granularity 0x%lx bytes, xnack: %s\n",
	       1UL << (svm->default_granularity + PAGE_SHIFT),
	       svm->xnack_enabled ? "enabled" : "disabled");

	vm->svm = svm;
	return 0;

err_free:
	kfree(svm);
	return ret;
}

int amdgpu_svm_init(struct amdgpu_device *adev, struct amdgpu_vm *vm)
{
	/* graphics svm init maybe different */

	return amdgpu_svm_init_compute(adev, vm, AMDGPU_SVM_XNACK_DEFAULT);
}

void amdgpu_svm_close(struct amdgpu_vm *vm)
{
	if (!vm->svm)
		return;

	if (atomic_xchg(&vm->svm->exiting, 1))
		return;

	amdgpu_svm_sync_work(vm->svm);
}

void amdgpu_svm_fini(struct amdgpu_vm *vm)
{
	struct amdgpu_svm *svm = vm->svm;

	if (!svm)
		return;

	amdgpu_svm_close(vm);
	amdgpu_svm_lock(svm);
	drm_gpusvm_fini(&svm->gpusvm);
	amdgpu_svm_unlock(svm);

	amdgpu_svm_attr_tree_destroy(svm->attr_tree);
	amdgpu_svm_work_fini(svm);
	vm->svm = NULL;
	amdgpu_svm_put(svm);
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

int amdgpu_svm_garbage_collector(struct amdgpu_svm *svm)
{
	struct amdgpu_svm_range_op_ctx op_ctx;

	amdgpu_svm_assert_locked(svm);

	spin_lock(&svm->work_lock);
	while (amdgpu_svm_range_dequeue_locked(svm, &svm->gc.list, &op_ctx)) {
		spin_unlock(&svm->work_lock);

		WARN_ON(!UNMAP_WORK(op_ctx.pending_ops));

		drm_gpusvm_range_remove(&svm->gpusvm,
					&op_ctx.range->base);

		amdgpu_svm_range_put_if_dequeued(svm, op_ctx.range);
		spin_lock(&svm->work_lock);
	}
	spin_unlock(&svm->work_lock);
	return 0;
}

void amdgpu_svm_clean_queue(struct amdgpu_svm *svm,
			   struct list_head *work_list)
{
	struct amdgpu_svm_range_op_ctx op_ctx;

	spin_lock(&svm->work_lock);
	while (amdgpu_svm_range_dequeue_locked(svm, work_list,
						 &op_ctx)) {
		spin_unlock(&svm->work_lock);
		amdgpu_svm_range_put_if_dequeued(svm, op_ctx.range);
		spin_lock(&svm->work_lock);
	}
	spin_unlock(&svm->work_lock);
}

static void amdgpu_svm_gc_work_func(struct work_struct *w)
{
	struct amdgpu_svm_gc *gc = container_of(w, struct amdgpu_svm_gc, work);
	struct amdgpu_svm *svm = container_of(gc, struct amdgpu_svm, gc);

	amdgpu_svm_lock(svm);
	amdgpu_svm_garbage_collector(svm);
	amdgpu_svm_unlock(svm);
}

static int amdgpu_svm_gc_init(struct amdgpu_svm *svm,
			      void (*gc_work_func)(struct work_struct *))
{
	svm->gc.wq = alloc_workqueue(AMDGPU_SVM_GC_WQ_NAME,
					WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM, 0);
	if (!svm->gc.wq)
		return -ENOMEM;

	INIT_LIST_HEAD(&svm->gc.list);
	INIT_WORK(&svm->gc.work, gc_work_func);

	return 0;
}

static void amdgpu_svm_gc_fini(struct amdgpu_svm *svm)
{
	flush_work(&svm->gc.work);
	amdgpu_svm_clean_queue(svm, &svm->gc.list);
	destroy_workqueue(svm->gc.wq);
	svm->gc.wq = NULL;
}

static void amdgpu_svm_gc_flush(struct amdgpu_svm *svm)
{
	flush_work(&svm->gc.work);
}

static int amdgpu_svm_work_init(struct amdgpu_svm *svm,
				void (*gc_work_func)(struct work_struct *))
{
	int ret;

	init_rwsem(&svm->svm_lock);
	spin_lock_init(&svm->work_lock);

	ret = amdgpu_svm_gc_init(svm, gc_work_func);
	if (ret)
		return ret;

	return 0;
}

static void amdgpu_svm_work_fini(struct amdgpu_svm *svm)
{
	amdgpu_svm_gc_fini(svm);
}

void amdgpu_svm_sync_work(struct amdgpu_svm *svm)
{
	amdgpu_svm_gc_flush(svm);
}

int amdgpu_gem_svm_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *filp)
{
	struct amdgpu_fpriv *fpriv = filp->driver_priv;
	struct amdgpu_device *adev = drm_to_adev(dev);
	struct drm_amdgpu_gem_svm *args = data;
	struct drm_amdgpu_svm_attribute *attrs = NULL;
	struct amdgpu_vm *vm;
	size_t attrs_size = 0;
	int ret = 0;

	AMDGPU_SVM_TRACE("ioctl op=%u va:[0x%llx-0x%llx)-0x%llx nattr=%u\n",
			 args->operation, args->start_addr, args->start_addr + args->size,
			 args->size, args->nattr);

	vm = &fpriv->vm;
	if (!amdgpu_svm_is_enabled(vm)) {
		ret = amdgpu_svm_init(adev, vm);
		if (ret)
			return ret;
	}

	if ((args->start_addr & ~PAGE_MASK) || (args->size & ~PAGE_MASK))
		return -EINVAL;

	if (!args->start_addr || !args->size)
		return -EINVAL;

	if (args->operation != AMDGPU_SVM_OP_RESET_ATTR) {
		ret = amdgpu_svm_copy_attrs(args, &attrs, &attrs_size);
		if (ret)
			return ret;
	}

	switch (args->operation) {
	case AMDGPU_SVM_OP_SET_ATTR:
		ret = amdgpu_svm_op_set_attr(vm, args->start_addr, args->size,
					 args->nattr, attrs);
		break;
	case AMDGPU_SVM_OP_GET_ATTR:
		ret = amdgpu_svm_op_get_attr(vm, args->start_addr, args->size,
					 args->nattr, attrs);
		if (!ret && copy_to_user(u64_to_user_ptr(args->attrs_ptr),
					 attrs, attrs_size))
			ret = -EFAULT;
		break;
	case AMDGPU_SVM_OP_RESET_ATTR:
		ret = amdgpu_svm_op_reset_attr(vm, args->start_addr, args->size);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	kvfree(attrs);
	return ret;
}

#endif /* CONFIG_DRM_AMDGPU_SVM */
