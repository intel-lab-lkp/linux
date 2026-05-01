// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <linux/kvm_host.h>

extern struct kvm_iommu_ops *kvm_nvhe_sym(kvm_iommu_ops);
extern unsigned int kvm_nvhe_sym(hyp_kvm_iommu_pages);

static DEFINE_MUTEX(kvm_iommu_reg_lock);

int kvm_iommu_register_driver(struct kvm_iommu_ops *hyp_ops, unsigned int pool_pages)
{
	guard(mutex)(&kvm_iommu_reg_lock);

	/* Only protected KVM before de-privilege. */
	if (!is_protected_kvm_enabled() || is_kvm_arm_initialised())
		return -EINVAL;

	if (kvm_nvhe_sym(kvm_iommu_ops))
		return -EBUSY;

	/* See kvm_iommu_pages() */
	if (pool_pages > kvm_nvhe_sym(hyp_kvm_iommu_pages)) {
		kvm_err("Not enough memory for the IOMMU pool, need 0x%x pages, check kvm-arm.hyp_iommu_pages",
			pool_pages);
		return -ENOMEM;
	}

	kvm_nvhe_sym(kvm_iommu_ops) = hyp_ops;
	return 0;
}

unsigned int kvm_iommu_pages(void)
{
	/*
	 * This is used very early during setup_arch() before any initcalls
	 * or any drivers are registered.
	 * This value is set by a command line option.
	 * Later, when the driver is registered, it will pass the number
	 * pages needed for it's page tables, if it was less that what
	 * the system has already allocated, the registration will fail.
	 */
	return kvm_nvhe_sym(hyp_kvm_iommu_pages);
}

/* Number of pages to reserve for iommu pool*/
static int __init early_hyp_iommu_pages(char *arg)
{
	if (!arg)
		return -EINVAL;

	return kstrtouint(arg, 0, &kvm_nvhe_sym(hyp_kvm_iommu_pages));
}
early_param("kvm-arm.hyp_iommu_pages", early_hyp_iommu_pages);
