/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_ARM_SMMU_V3_H
#define __KVM_ARM_SMMU_V3_H

#include <asm/kvm_asm.h>

#ifdef __KVM_NVHE_HYPERVISOR__
#include <nvhe/spinlock.h>
#endif

#include "../arm-smmu-v3-common.h"

/*
 * Parameters from the trusted host:
 * @mmio_addr		base address of the SMMU registers
 * @mmio_size		size of the registers resource
 * @base		Virtual address of SMMU registers
 * @features		SMMUv3 features as defined in arm-smmu-v3-common.h
 * @cmdq		CMDQ queue struct
 * @strtab_cfg		stream table config, strtab_cfg.l2.l2ptrs is not used
 * @ias			IAS of the SMMUv3
 * @oas			OAS of the SMMUv3
 * @pgsize_bitmap	Pages sizes supported by the SMMUv3
 * Other members are filled and used at runtime by the SMMU driver.
 * @lock		Lock to protect the SMMU resources (STE/CMDQ)
 */
struct hyp_arm_smmu_v3_device {
	phys_addr_t		mmio_addr;
	size_t			mmio_size;
	void __iomem		*base;
	unsigned long		features;
	struct arm_smmu_queue	cmdq;
	struct arm_smmu_strtab_cfg strtab_cfg;
	unsigned int            ias;
	unsigned int            oas;
	size_t                  pgsize_bitmap;
	/* nvhe/spinlock.h not exposed to EL1. */
#ifdef __KVM_NVHE_HYPERVISOR__
	hyp_spinlock_t		lock;
#else
	u32			lock;
#endif
};

extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count);
#define kvm_hyp_arm_smmu_v3_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count)

extern struct hyp_arm_smmu_v3_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus);
#define kvm_hyp_arm_smmu_v3_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus)

#endif /* __KVM_ARM_SMMU_V3_H */
