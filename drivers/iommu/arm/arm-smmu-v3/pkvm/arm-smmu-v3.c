// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>

#include "arm-smmu-v3-hyp.h"
#include "../arm-smmu-v3.h"
#include "../arm-smmu-v3-common-lib.h"

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

/* Put the device in a state that can be probed by the host driver. */
static void smmu_deinit_device(struct hyp_arm_smmu_v3_device *smmu)
{
	WARN_ON(__pkvm_hyp_donate_host_mmio(hyp_phys_to_pfn(smmu->mmio_addr),
					    smmu->mmio_size >> PAGE_SHIFT));
	smmu->base = NULL;
}

/*
 * Mini-probe and validation for the hypervisor.
 */
static int smmu_probe(struct hyp_arm_smmu_v3_device *smmu)
{
	u32 reg;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		return -EINVAL;

	reg = arm_smmu_idr0_probe(smmu);

	if (!(FIELD_GET(IDR0_TTF, reg) & IDR0_TTF_AARCH64))
		return -ENXIO;

	if (!(smmu->features & (ARM_SMMU_FEAT_TT_LE | ARM_SMMU_FEAT_TT_BE)))
		return -ENXIO;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR1);
	if (reg & (IDR1_TABLES_PRESET | IDR1_QUEUES_PRESET | IDR1_REL))
		return -EINVAL;

	smmu->sid_bits = FIELD_GET(IDR1_SIDSIZE, reg);
	/* Follows the kernel logic */
	if (smmu->sid_bits <= STRTAB_SPLIT)
		smmu->features &= ~ARM_SMMU_FEAT_2_LVL_STRTAB;

	arm_smmu_idr3_probe(smmu);

	arm_smmu_idr5_probe(smmu);
	if (!smmu->oas)
		smmu->oas = 48;

	arm_smmu_device_iidr_probe(smmu);

	if (!(smmu->features & ARM_SMMU_FEAT_NESTING))
		return -ENXIO;

	return 0;
}

static int smmu_init_device(struct hyp_arm_smmu_v3_device *smmu)
{
	unsigned long haddr;
	int ret;

	if (!PAGE_ALIGNED(smmu->mmio_addr | smmu->mmio_size))
		return -EINVAL;

	ret = __pkvm_host_donate_hyp_mmio(hyp_phys_to_pfn(smmu->mmio_addr),
					  smmu->mmio_size >> PAGE_SHIFT, &haddr);
	if (ret)
		return ret;

	smmu->base = (void __iomem *)haddr;
	ret = smmu_probe(smmu);
	if (ret)
		goto out_ret;

	return 0;
out_ret:
	smmu_deinit_device(smmu);
	return ret;
}

/* Called while is the host is still trusted. */
static int smmu_init(void)
{
	size_t smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_smmus) *
					  kvm_hyp_arm_smmu_v3_count);
	struct hyp_arm_smmu_v3_device *smmu;
	u64 pfn, nr_pages;
	int ret;

	kvm_hyp_arm_smmu_v3_smmus = kern_hyp_va(kvm_hyp_arm_smmu_v3_smmus);
	pfn = hyp_virt_to_pfn(kvm_hyp_arm_smmu_v3_smmus);
	nr_pages = smmu_arr_size >> PAGE_SHIFT;

	ret = __pkvm_host_donate_hyp(pfn, nr_pages);
	if (ret)
		return ret;

	for_each_smmu(smmu) {
		ret = smmu_init_device(smmu);
		if (ret)
			goto out_reclaim_smmu;
	}

	return 0;

out_reclaim_smmu:
	while (smmu != kvm_hyp_arm_smmu_v3_smmus)
		smmu_deinit_device(--smmu);
	WARN_ON(__pkvm_hyp_donate_host(pfn, nr_pages));
	return ret;
}

static int smmu_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
	return 0;
}

/* Shared with the kernel driver in EL1 */
struct pkvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.host_stage2_idmap		= smmu_host_stage2_idmap,
};
