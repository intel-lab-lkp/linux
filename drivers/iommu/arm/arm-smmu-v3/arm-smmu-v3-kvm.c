// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/auxiliary_bus.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "arm-smmu-v3.h"
#include "pkvm/arm_smmu_v3.h"

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;
static size_t				kvm_arm_smmu_cur;

static void kvm_arm_smmu_array_free(void)
{
	int order;

	order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	free_pages((unsigned long)kvm_arm_smmu_array, order);
}

static int kvm_arm_smmu_array_alloc(void)
{
	int smmu_order;
	struct device_node *np;

	for_each_compatible_node(np, NULL, "arm,smmu-v3")
		kvm_arm_smmu_count++;

	if (!kvm_arm_smmu_count)
		return -ENODEV;
	smmu_order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	kvm_arm_smmu_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, smmu_order);
	if (!kvm_arm_smmu_array)
		return -ENOMEM;
	return 0;
}

static unsigned int smmu_hyp_pgt_pages(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "arm,smmu-v3");

	/*
	 * SMMUv3 uses the same format as the CPU stage-2 and hence have the same memory
	 * requirements, we add extra 500 pages for L2 STEs.
	 * Only one set of memory is allocated as the page table is shared between all
	 * the SMMUs.
	 */
	if (np) {
		of_node_put(np);
		return host_s2_pgtable_pages() + 500;
	}

	return 0;
}

static struct platform_driver smmuv3_nesting_driver;
static int smmuv3_nesting_probe(struct platform_device *pdev)
{
	struct hyp_arm_smmu_v3_device *smmu = &kvm_arm_smmu_array[kvm_arm_smmu_cur];
	struct device *dev = &pdev->dev;
	struct resource *res;

	/* Only device tree, ACPI not supported. */
	if (!dev->of_node)
		return -EINVAL;

	if (kvm_arm_smmu_cur >= kvm_arm_smmu_count)
		return -ENOSPC;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	if (of_property_read_bool(dev->of_node, "cavium,cn9900-broken-page1-regspace"))
		return -EINVAL;

	smmu->mmio_addr = res->start;
	smmu->mmio_size = resource_size(res);
	if (smmu->mmio_size < SZ_128K) {
		dev_err(dev, "MMIO region too small(%pr)\n", res);
		return -EINVAL;
	}

	if (of_dma_is_coherent(dev->of_node))
		smmu->features |= ARM_SMMU_FEAT_COHERENCY;

	kvm_arm_smmu_cur++;
	return 0;
}

static int kvm_arm_smmu_v3_register(void)
{
	size_t nr_pages = smmu_hyp_pgt_pages();
	int ret;

	if (!is_protected_kvm_enabled() || !nr_pages)
		return 0;

	ret = kvm_arm_smmu_array_alloc();
	if (ret)
		goto out_err;

	ret = platform_driver_probe(&smmuv3_nesting_driver, smmuv3_nesting_probe);
	if (ret)
		goto out_free;

	ret = kvm_iommu_register_driver(kern_hyp_va(lm_alias(&kvm_nvhe_sym(smmu_ops))),
					nr_pages);
	if (ret)
		goto out_unregister;

	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 */
	kvm_hyp_arm_smmu_v3_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_count = kvm_arm_smmu_cur;
	return ret;

out_unregister:
	platform_driver_unregister(&smmuv3_nesting_driver);
out_free:
	kvm_arm_smmu_array_free();
out_err:
	kvm_arm_smmu_count = 0;
	kvm_arm_smmu_array = NULL;
	return ret;
};

static int smmu_create_aux_device(struct device *dev, void *data)
{
	static int dev_id;
	struct auxiliary_device *auxdev;

	auxdev = __devm_auxiliary_device_create(dev, "protected_kvm",
						"smmu_v3_emu", NULL, dev_id++);
	if (!auxdev)
		return -ENODEV;

	auxdev->dev.parent = dev;
	return 0;
}

static int kvm_arm_smmu_v3_post_init(void)
{
	if (!kvm_arm_smmu_count)
		return 0;

	/*
	 * If the hypervisor part of the driver fails, KVM will not initialise.
	 */
	if (!is_kvm_arm_initialised()) {
		kvm_arm_smmu_array_free();
		return 0;
	}

	WARN_ON(driver_for_each_device(&smmuv3_nesting_driver.driver, NULL,
				       NULL, smmu_create_aux_device));

	return 0;
}

static const struct of_device_id smmuv3_nested_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver smmuv3_nesting_driver = {
	.driver = {
		.name = "smmuv3-nesting",
		.of_match_table = smmuv3_nested_of_match,
		.suppress_bind_attrs = true,
	},
};
late_initcall(kvm_arm_smmu_v3_post_init);
subsys_initcall(kvm_arm_smmu_v3_register);
