// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <linux/kvm_host.h>

extern struct kvm_iommu_ops *kvm_nvhe_sym(kvm_iommu_ops);
extern size_t kvm_nvhe_sym(hyp_kvm_iommu_pages);

int kvm_iommu_register_driver(struct kvm_iommu_ops *hyp_ops)
{
	kvm_nvhe_sym(kvm_iommu_ops) = hyp_ops;
	return 0;
}

size_t kvm_iommu_pages(void)
{
	/*
	 * This is called very early during setup_arch() where no initcalls,
	 * so this has to call specific functions per each KVM driver.
	 */
	kvm_nvhe_sym(hyp_kvm_iommu_pages) = 0;
	return 0;
}
