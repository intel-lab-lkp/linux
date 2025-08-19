// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/of_address.h>
#include <linux/of_platform.h>

#include "arm-smmu-v3.h"
#include "pkvm/arm_smmu_v3.h"

#define SMMU_KVM_CMDQ_ORDER				4
#define SMMU_KVM_STRTAB_ORDER				(get_order(STRTAB_MAX_L1_ENTRIES * \
							 sizeof(struct arm_smmu_strtab_l1)))

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;

static void kvm_arm_smmu_array_free(void)
{
	int order;

	order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	free_pages((unsigned long)kvm_arm_smmu_array, order);
}

/*
 * The hypervisor have to know the basic information about the SMMUs
 * from the firmware.
 * This has to be done before the SMMUv3 probes and does anything meaningful
 * with the hardware, otherwise it becomes harder to reason about the SMMU
 * state and we'd require to hand-off the state to the hypervisor at certain point
 * while devices are live, which is complicated and dangerous.
 * Instead, the hypervisor is interested in a very small part of the probe path,
 * so just add a separate logic for it.
 */
static int kvm_arm_smmu_array_alloc(void)
{
	int smmu_order;
	struct device_node *np;
	int ret;
	int i = 0;

	kvm_arm_smmu_count = 0;
	for_each_compatible_node(np, NULL, "arm,smmu-v3")
		kvm_arm_smmu_count++;

	if (!kvm_arm_smmu_count)
		return -ENODEV;

	smmu_order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	kvm_arm_smmu_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, smmu_order);
	if (!kvm_arm_smmu_array)
		return -ENOMEM;

	/* Basic device tree parsing. */
	for_each_compatible_node(np, NULL, "arm,smmu-v3") {
		struct resource res;
		void *cmdq_base, *strtab;

		ret = of_address_to_resource(np, 0, &res);
		if (ret)
			goto out_err;
		kvm_arm_smmu_array[i].mmio_addr = res.start;
		kvm_arm_smmu_array[i].mmio_size = resource_size(&res);
		if (kvm_arm_smmu_array[i].mmio_size < SZ_128K) {
			pr_err("SMMUv3(%s) has unsupported size(0x%lx)\n", np->name,
			       kvm_arm_smmu_array[i].mmio_size);
			ret = -EINVAL;
			goto out_err;
		}

		if (of_dma_is_coherent(np))
			kvm_arm_smmu_array[i].features |= ARM_SMMU_FEAT_COHERENCY;

		/*
		 * Allocate shadow for the command queue, it doesn't have to be the same
		 * size as the host.
		 * Only populate base_dma and llq.max_n_shift, the hypervisor will init
		 * the rest.
		 * We don't what size the host will choose at this point, the shadow copy
		 * will 64K which is a reasonable size.
		 */
		cmdq_base = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, SMMU_KVM_CMDQ_ORDER);
		if (!cmdq_base) {
			ret = -ENOMEM;
			goto out_err;
		}

		kvm_arm_smmu_array[i].cmdq.base_dma = virt_to_phys(cmdq_base);
		kvm_arm_smmu_array[i].cmdq.llq.max_n_shift = SMMU_KVM_CMDQ_ORDER + PAGE_SHIFT -
							     CMDQ_ENT_SZ_SHIFT;

		strtab = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, SMMU_KVM_STRTAB_ORDER);
		if (!strtab) {
			ret = -ENOMEM;
			goto out_err;
		}
		kvm_arm_smmu_array[i].strtab_dma = virt_to_phys(strtab);
		kvm_arm_smmu_array[i].strtab_size = PAGE_SIZE << SMMU_KVM_STRTAB_ORDER;

		i++;
	}

	return 0;

out_err:
	kvm_arm_smmu_array_free();
	return ret;
}

size_t smmu_hyp_pgt_pages(void)
{
	/*
	 * SMMUv3 uses the same format as stage-2 and hence have the same memory
	 * requirements, we add extra 500 pages for L2 ste.
	 */
	if (of_find_compatible_node(NULL, NULL, "arm,smmu-v3"))
		return host_s2_pgtable_pages() + 500;
	return 0;
}

static int kvm_arm_smmu_v3_register(void)
{
	int ret;

	if (!is_protected_kvm_enabled())
		return 0;

	ret = kvm_arm_smmu_array_alloc();
	if (ret)
		return ret;

	ret = kvm_iommu_register_driver(kern_hyp_va(lm_alias(&kvm_nvhe_sym(smmu_ops))));
	if (ret)
		goto out_err;

	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 */
	kvm_hyp_arm_smmu_v3_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_count = kvm_arm_smmu_count;
	return ret;

out_err:
	kvm_arm_smmu_array_free();
	return ret;
};

core_initcall(kvm_arm_smmu_v3_register);
