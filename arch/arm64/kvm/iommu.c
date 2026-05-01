// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <linux/kvm_host.h>

extern struct kvm_iommu_ops *kvm_nvhe_sym(kvm_iommu_ops);

static DEFINE_MUTEX(kvm_iommu_reg_lock);

int kvm_iommu_register_driver(struct kvm_iommu_ops *hyp_ops)
{
	guard(mutex)(&kvm_iommu_reg_lock);

	/* Only protected KVM before de-privilege. */
	if (!is_protected_kvm_enabled() || is_kvm_arm_initialised())
		return -EINVAL;

	if (kvm_nvhe_sym(kvm_iommu_ops))
		return -EBUSY;

	kvm_nvhe_sym(kvm_iommu_ops) = hyp_ops;
	return 0;
}
