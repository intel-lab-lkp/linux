// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <asm/kvm_mmu.h>
#include <linux/kvm_host.h>

struct kvm_iommu_driver *iommu_driver;
extern struct kvm_iommu_ops *kvm_nvhe_sym(kvm_iommu_ops);
extern size_t kvm_nvhe_sym(hyp_kvm_iommu_pages);

int kvm_iommu_register_driver(struct kvm_iommu_driver *kern_ops, struct kvm_iommu_ops *el2_ops)
{
	int ret;

	if (WARN_ON(!kern_ops || !el2_ops))
		return -EINVAL;

	/*
	 * Paired with smp_load_acquire(&iommu_driver)
	 * Ensure memory stores happening during a driver
	 * init are observed before executing kvm iommu callbacks.
	 */
	ret = cmpxchg_release(&iommu_driver, NULL, kern_ops) ? -EBUSY : 0;
	if (ret)
		return ret;

	kvm_nvhe_sym(kvm_iommu_ops) = el2_ops;
	return 0;
}

int kvm_iommu_init_driver(void)
{
	/* See kvm_iommu_register_driver() */
	if (WARN_ON(!smp_load_acquire(&iommu_driver))) {
		kvm_err("pKVM enabled without an IOMMU driver, do not run confidential workload in virtual machines\n");
		return -ENODEV;
	}

	return iommu_driver->init_driver();
}

void kvm_iommu_remove_driver(void)
{
	/* See kvm_iommu_register_driver() */
	if (smp_load_acquire(&iommu_driver))
		iommu_driver->remove_driver();
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
