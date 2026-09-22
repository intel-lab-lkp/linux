// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <linux/kvm_host.h>

extern struct pkvm_iommu_ops *kvm_nvhe_sym(pkvm_iommu_ops);
extern unsigned int kvm_nvhe_sym(hyp_kvm_iommu_pages);

static DEFINE_MUTEX(pkvm_iommu_reg_lock);

int pkvm_iommu_register_driver(struct pkvm_iommu_ops *hyp_ops, unsigned int nr_pages)
{
	guard(mutex)(&pkvm_iommu_reg_lock);

	if (!is_protected_kvm_enabled() || is_kvm_arm_initialised())
		return -EPERM;

	if (kvm_nvhe_sym(pkvm_iommu_ops))
		return -EBUSY;

	/* See pkvm_iommu_pages() */
	if (nr_pages > kvm_nvhe_sym(hyp_kvm_iommu_pages)) {
		kvm_err("IOMMU pool needs 0x%x pages, check kvm-arm.iommu_pgt_mem\n", nr_pages);
		return -ENOMEM;
	}

	kvm_nvhe_sym(pkvm_iommu_ops) = hyp_ops;
	return 0;
}

unsigned int pkvm_iommu_pages(void)
{
	/*
	 * This is used very early during setup_arch() before any initcalls
	 * or any drivers are registered.
	 * This value is set by a command line option.
	 * Later, when the driver is registered, it will pass the number
	 * pages needed for it's page tables, if it was more than what
	 * the system has already allocated, it will fail registration.
	 */
	return kvm_nvhe_sym(hyp_kvm_iommu_pages);
}

static int __init early_iommu_pgt_mem(char *arg)
{
	unsigned long long requested_size;

	if (!arg)
		return -EINVAL;

	requested_size = memparse(arg, NULL);

	if (requested_size > UINT_MAX) {
		kvm_err("kvm-arm.iommu_pgt_mem is too large\n");
		return -EINVAL;
	}

	kvm_nvhe_sym(hyp_kvm_iommu_pages) = DIV_ROUND_UP(requested_size, PAGE_SIZE);
	return 0;
}
early_param("kvm-arm.iommu_pgt_mem", early_iommu_pgt_mem);
