/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_VM_MADVISE_H_
#define _XE_VM_MADVISE_H_

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct xe_bo;
struct xe_vm;
struct xe_vma;

int xe_vm_madvise_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file);

int xe_vm_madvise_init(struct xe_vm *vm);
void xe_vm_madvise_fini(struct xe_vm *vm);
int xe_vm_madvise_register_notifier_range(struct xe_vm *vm, u64 start, u64 end);

#endif
