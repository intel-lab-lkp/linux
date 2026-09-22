/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_ARM_SMMU_V3_HYP_H
#define __KVM_ARM_SMMU_V3_HYP_H

#include <asm/kvm_asm.h>

#ifdef __KVM_NVHE_HYPERVISOR__
#include <nvhe/spinlock.h>
#endif

#include "../arm-smmu-v3.h"

/*
 * Parameters from the trusted host:
 * @mmio_addr		base address of the SMMU registers
 * @mmio_size		size of the registers resource
 * @strtab_dma		Phys address of stream table
 * @strtab_size		Stream table size
 *
 * Other members are filled and used at runtime by the SMMU driver.
 * @base		Virtual address of SMMU registers
 * @oas			PA size
 * @pgsize_bitmap	Supported page sizes
 * @sid_bits		Max number of SID bits supported
 * @lock		Lock to protect SMMU emulation
 * @hw_lock		Lock to protect SMMU HW (as CMDQ)
 *			Order smmu.lock => host_mmu.lock => smmu.hw_lock
 * @cmdq		CMDQ as observed by HW
 * @cmdq_host		Host view of the CMDQ, only q_base and llq used.
 * @cmdq_max_shift	Max shift for the CMDQ probed from HW.
 * @cr0			Last value of CR0
 * @cmdq_active		Is SMMU HW cmdq usable, protected by hw_lock
 * @host_ste_cfg	Host stream table config
 * @host_ste_base	Host stream table base
 * @strtab_cfg		Stream table as seen by HW
 * @evtq_base		Host evtq base reg
 * @priq_base		Host priq base reg
 * @evtq_shared		Whether the EVTQ was shared
 * @priq_shared		Whether the PRIQ was shared
 */
struct hyp_arm_smmu_v3_device {
	phys_addr_t		mmio_addr;
	size_t			mmio_size;
	void __iomem		*base;
	u32			features;
	u32			options;
	unsigned long		oas;
	unsigned long		pgsize_bitmap;
	unsigned int		sid_bits;
#ifdef __KVM_NVHE_HYPERVISOR__
	hyp_spinlock_t		lock;
	hyp_spinlock_t		hw_lock;
#else
	u32			lock;
	u32			hw_lock;
#endif
	struct arm_smmu_queue	cmdq;
	struct arm_smmu_queue	cmdq_host;
	u32			cmdq_max_shift;
	u32			cr0;
	bool			cmdq_active;
	dma_addr_t		strtab_dma;
	size_t			strtab_size;
	u64			host_ste_cfg;
	u64			host_ste_base;
	struct arm_smmu_strtab_cfg strtab_cfg;
	u64			evtq_base;
	u64			priq_base;
	bool			evtq_shared;
	bool			priq_shared;
};

extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count);
#define kvm_hyp_arm_smmu_v3_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count)

extern struct hyp_arm_smmu_v3_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus);
#define kvm_hyp_arm_smmu_v3_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus)

#endif /* __KVM_ARM_SMMU_V3_HYP_H */
