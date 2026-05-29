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
#include <drm/drm_pagemap.h>
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
struct amdgpu_svm_attrs;
struct drm_device;
struct drm_file;

#define AMDGPU_INTERCONNECT_VRAM	DRM_INTERCONNECT_DRIVER
#define AMDGPU_INTERCONNECT_P2P		(AMDGPU_INTERCONNECT_VRAM + 1)

enum amdgpu_svm_xnack_mode {
	AMDGPU_SVM_XNACK_OFF,
	AMDGPU_SVM_XNACK_ON,
	AMDGPU_SVM_XNACK_DEFAULT,
};

#define AMDGPU_SVM_TRACE(fmt, ...) \
	pr_debug("%s: " fmt, __func__, ##__VA_ARGS__)

#define AMDGPU_SVM_WARN(fmt, ...) \
	pr_warn("%s: " fmt, __func__, ##__VA_ARGS__)

#define AMDGPU_SVM_ERR(fmt, ...) \
	pr_err("%s: " fmt, __func__, ##__VA_ARGS__)

#define AMDGPU_SVM_KMEM_CACHE_CREATE(name, type) \
	kmem_cache_create((name), sizeof(type), 0, 0, NULL)

#define AMDGPU_SVM_KMEM_CACHE_DESTROY(cache) \
	do { \
		if ((cache) != NULL) { \
			kmem_cache_destroy((cache)); \
			(cache) = NULL; \
		} \
	} while (0)

#define amdgpu_svm_assert_in_notifier(svm__) \
	lockdep_assert_held_write(&(svm__)->gpusvm.notifier_lock)

struct amdgpu_svm_gc {
	struct workqueue_struct *wq;
	struct list_head list;
	struct work_struct work;
};

struct amdgpu_svm {
	struct drm_gpusvm gpusvm;
	struct kref refcount;
	struct amdgpu_device *adev;
	struct amdgpu_vm *vm;
	struct amdgpu_svm_attr_tree *attr_tree;
	struct rw_semaphore svm_lock;
	spinlock_t work_lock;
	struct amdgpu_svm_gc gc;
	atomic_t exiting;
	uint64_t checkpoint_ts;
	u8 default_granularity;
	bool xnack_enabled;
	void (*flush_tlb)(struct amdgpu_svm *svm);
	void (*invalidate_ranges)(struct amdgpu_svm *svm,
				  struct drm_gpusvm_notifier *notifier,
				  const struct mmu_notifier_range *mmu_range,
				  struct drm_gpusvm_range *first,
				  uint64_t adj_start, uint64_t adj_end);
};

static inline struct amdgpu_svm *to_amdgpu_svm(struct drm_gpusvm *gpusvm)
{
	return container_of(gpusvm, struct amdgpu_svm, gpusvm);
}

static inline void amdgpu_svm_lock(struct amdgpu_svm *svm)
{
	down_write(&svm->svm_lock);
}

static inline void amdgpu_svm_unlock(struct amdgpu_svm *svm)
{
	up_write(&svm->svm_lock);
}

static inline void amdgpu_svm_assert_locked(struct amdgpu_svm *svm)
{
	lockdep_assert_held_write(&svm->svm_lock);
}

#if IS_ENABLED(CONFIG_DRM_AMDGPU_SVM)
int amdgpu_svm_cache_init(void);
void amdgpu_svm_cache_fini(void);

int amdgpu_svm_init(struct amdgpu_device *adev, struct amdgpu_vm *vm);
void amdgpu_svm_close(struct amdgpu_vm *vm);
void amdgpu_svm_fini(struct amdgpu_vm *vm);

void amdgpu_svm_put(struct amdgpu_svm *svm);
struct amdgpu_svm *amdgpu_svm_lookup_by_pasid(struct amdgpu_device *adev,
					       uint32_t pasid);
int amdgpu_svm_handle_fault(struct amdgpu_device *adev, uint32_t pasid,
			    uint64_t fault_page, uint64_t ts,
			    bool write_fault);
bool amdgpu_svm_is_enabled(struct amdgpu_vm *vm);

int amdgpu_gem_svm_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *filp);
void amdgpu_svm_clean_queue(struct amdgpu_svm *svm,
			    struct list_head *work_list);
void amdgpu_svm_sync_work(struct amdgpu_svm *svm);
int amdgpu_svm_garbage_collector(struct amdgpu_svm *svm);
int amdgpu_svm_apply_attr_change(struct amdgpu_svm *svm,
				 const struct amdgpu_svm_attrs *old_attrs,
				 const struct amdgpu_svm_attrs *new_attrs,
				 unsigned long start_page,
				 unsigned long last_page);
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
					  uint64_t fault_page,
					  uint64_t ts,
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
