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

#ifndef __AMDGPU_SVM_H__
#define __AMDGPU_SVM_H__

#include <drm/amdgpu_drm.h>
#include <drm/drm_gpusvm.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct amdgpu_device;
struct amdgpu_vm;
struct amdgpu_svm_attr_tree;
struct drm_device;
struct drm_file;

#define AMDGPU_SVM_TRACE(fmt, ...) \
	pr_debug("%s: " fmt, __func__, ##__VA_ARGS__)

#define AMDGPU_SVM_KMEM_CACHE_CREATE(name, type) \
	kmem_cache_create((name), sizeof(type), 0, 0, NULL)

#define AMDGPU_SVM_KMEM_CACHE_DESTROY(cache) \
	do { \
		if ((cache) != NULL) { \
			kmem_cache_destroy((cache)); \
			(cache) = NULL; \
		} \
	} while (0)

struct amdgpu_svm {
	struct drm_gpusvm gpusvm;
	struct kref refcount;
	struct amdgpu_device *adev;
	struct amdgpu_vm *vm;
	struct amdgpu_svm_attr_tree *attr_tree;
	struct workqueue_struct *gc_wq;
	struct workqueue_struct *restore_wq;
	struct rw_semaphore svm_lock;
	spinlock_t gc_lock;
	struct list_head gc_list;
	struct work_struct gc_work;
	struct list_head restore_work_list;
	struct delayed_work restore_work;
	atomic_t kfd_queues_quiesced;
	atomic_t evicted_ranges;
	atomic_t exiting;
	u8 default_granularity;
	bool xnack_enabled;
	void (*begin_restore)(struct amdgpu_svm *svm);
	void (*end_restore)(struct amdgpu_svm *svm);
	void (*flush_tlb)(struct amdgpu_svm *svm);
};

static inline struct amdgpu_svm *to_amdgpu_svm(struct drm_gpusvm *gpusvm)
{
	return container_of(gpusvm, struct amdgpu_svm, gpusvm);
}

#if IS_ENABLED(CONFIG_DRM_AMDGPU_SVM)
int amdgpu_svm_cache_init(void);
void amdgpu_svm_cache_fini(void);

int amdgpu_svm_init(struct amdgpu_device *adev, struct amdgpu_vm *vm);
void amdgpu_svm_close(struct amdgpu_vm *vm);
void amdgpu_svm_fini(struct amdgpu_vm *vm);

int amdgpu_svm_handle_fault(struct amdgpu_device *adev, uint32_t pasid,
			    uint64_t fault_addr, bool write_fault);
bool amdgpu_svm_is_enabled(struct amdgpu_vm *vm);

int amdgpu_gem_svm_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *filp);
#else
static inline int amdgpu_svm_init(struct amdgpu_device *adev,
				  struct amdgpu_vm *vm)
{
	return 0;
}

static inline int amdgpu_svm_cache_init(void)
{
	return 0;
}

static inline void amdgpu_svm_cache_fini(void)
{
}

static inline void amdgpu_svm_close(struct amdgpu_vm *vm)
{
}

static inline void amdgpu_svm_fini(struct amdgpu_vm *vm)
{
}

static inline int amdgpu_svm_handle_fault(struct amdgpu_device *adev,
					  uint32_t pasid,
					  uint64_t fault_addr,
					  bool write_fault)
{
	return -EOPNOTSUPP;
}

static inline bool amdgpu_svm_is_enabled(struct amdgpu_vm *vm)
{
	return false;
}

static inline int amdgpu_gem_svm_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *filp)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_DRM_AMDGPU_SVM */

#endif /* __AMDGPU_SVM_H__ */
