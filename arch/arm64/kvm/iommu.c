// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <linux/kvm_host.h>

extern struct pkvm_iommu_ops *kvm_nvhe_sym(pkvm_iommu_ops);

static DEFINE_MUTEX(pkvm_iommu_reg_lock);

int pkvm_iommu_register_driver(struct pkvm_iommu_ops *hyp_ops)
{
	guard(mutex)(&pkvm_iommu_reg_lock);

	if (!is_protected_kvm_enabled() || is_kvm_arm_initialised())
		return -EPERM;

	if (kvm_nvhe_sym(pkvm_iommu_ops))
		return -EBUSY;

	kvm_nvhe_sym(pkvm_iommu_ops) = hyp_ops;
	return 0;
}
