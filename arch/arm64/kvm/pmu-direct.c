// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/perf/arm_pmu.h>
#include <linux/perf/arm_pmuv3.h>

#include <asm/arm_pmuv3.h>
#include <asm/kvm_pmu.h>

/**
 * kvm_pmu_partition_supported() - Determine if partitioning is possible
 *
 * Partitioning is only supported in VHE mode with PMUv3
 *
 * Return: True if partitioning is possible, false otherwise
 */
bool kvm_pmu_partition_supported(void)
{
	return has_vhe() &&
		system_supports_pmuv3();
}

/**
 * kvm_pmu_is_partitioned() - Determine if given PMU is partitioned
 * @pmu: Pointer to arm_pmu struct
 *
 * Determine if given PMU is partitioned by looking at hpmn field. The
 * PMU is partitioned if this field is less than the number of
 * counters in the system.
 *
 * Return: True if the PMU is partitioned, false otherwise
 */
bool kvm_pmu_is_partitioned(struct arm_pmu *pmu)
{
	if (!pmu)
		return false;

	return pmu->hpmn_max >= 0 &&
		pmu->hpmn_max <= *host_data_ptr(nr_event_counters);
}

/**
 * kvm_vcpu_pmu_is_partitioned() - Determine if given VCPU has a partitioned PMU
 * @vcpu: Pointer to kvm_vcpu struct
 *
 * Determine if given VCPU has a partitioned PMU by extracting that
 * field and passing it to :c:func:`kvm_pmu_is_partitioned`
 *
 * Return: True if the VCPU PMU is partitioned, false otherwise
 */
bool kvm_vcpu_pmu_is_partitioned(struct kvm_vcpu *vcpu)
{
	return kvm_pmu_is_partitioned(vcpu->kvm->arch.arm_pmu) &&
		false;
}

/**
 * kvm_vcpu_pmu_use_fgt() - Determine if we can use FGT
 * @vcpu: Pointer to struct kvm_vcpu
 *
 * Determine if we can use FGT for direct access to registers. We can
 * if capabilities permit the number of guest counters requested.
 *
 * Return: True if we can use FGT, false otherwise
 */
bool kvm_vcpu_pmu_use_fgt(struct kvm_vcpu *vcpu)
{
	u8 hpmn = vcpu->kvm->arch.nr_pmu_counters;

	return kvm_vcpu_pmu_is_partitioned(vcpu) &&
		cpus_have_final_cap(ARM64_HAS_FGT) &&
		(hpmn != 0 || cpus_have_final_cap(ARM64_HAS_HPMN0));
}

u64 kvm_pmu_fgt_bits(void)
{
	return HDFGRTR_EL2_PMOVS
		| HDFGRTR_EL2_PMCCFILTR_EL0
		| HDFGRTR_EL2_PMEVTYPERn_EL0
		| HDFGRTR_EL2_PMCEIDn_EL0
		| HDFGRTR_EL2_PMMIR_EL1;
}

u64 kvm_pmu_fgt2_bits(void)
{
	return HDFGRTR2_EL2_nPMICFILTR_EL0
		| HDFGRTR2_EL2_nPMICNTR_EL0;
}

/**
 * kvm_pmu_host_counter_mask() - Compute bitmask of host-reserved counters
 * @pmu: Pointer to arm_pmu struct
 *
 * Compute the bitmask that selects the host-reserved counters in the
 * {PMCNTEN,PMINTEN,PMOVS}{SET,CLR} registers. These are the counters
 * in HPMN..N
 *
 * Return: Bitmask
 */
u64 kvm_pmu_host_counter_mask(struct arm_pmu *pmu)
{
	u8 nr_counters = *host_data_ptr(nr_event_counters);

	if (!kvm_pmu_is_partitioned(pmu))
		return ARMV8_PMU_CNT_MASK_ALL;

	return GENMASK(nr_counters - 1, pmu->hpmn_max);
}

/**
 * kvm_pmu_guest_counter_mask() - Compute bitmask of guest-reserved counters
 *
 * Compute the bitmask that selects the guest-reserved counters in the
 * {PMCNTEN,PMINTEN,PMOVS}{SET,CLR} registers. These are the counters
 * in 0..HPMN and the cycle and instruction counters.
 *
 * Return: Bitmask
 */
u64 kvm_pmu_guest_counter_mask(struct arm_pmu *pmu)
{
	return ARMV8_PMU_CNT_MASK_ALL & ~kvm_pmu_host_counter_mask(pmu);
}

/**
 * kvm_pmu_host_counters_enable() - Enable host-reserved counters
 *
 * When partitioned the enable bit for host-reserved counters is
 * MDCR_EL2.HPME instead of the typical PMCR_EL0.E, which now
 * exclusively controls the guest-reserved counters. Enable that bit.
 */
void kvm_pmu_host_counters_enable(void)
{
	u64 mdcr = read_sysreg(mdcr_el2);

	mdcr |= MDCR_EL2_HPME;
	write_sysreg(mdcr, mdcr_el2);
}

/**
 * kvm_pmu_host_counters_disable() - Disable host-reserved counters
 *
 * When partitioned the disable bit for host-reserved counters is
 * MDCR_EL2.HPME instead of the typical PMCR_EL0.E, which now
 * exclusively controls the guest-reserved counters. Disable that bit.
 */
void kvm_pmu_host_counters_disable(void)
{
	u64 mdcr = read_sysreg(mdcr_el2);

	mdcr &= ~MDCR_EL2_HPME;
	write_sysreg(mdcr, mdcr_el2);
}
