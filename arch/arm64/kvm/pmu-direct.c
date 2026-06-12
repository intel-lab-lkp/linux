// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/perf/arm_pmu.h>
#include <linux/perf/arm_pmuv3.h>

#include <asm/arm_pmuv3.h>
#include <asm/kvm_emulate.h>

/**
 * has_host_pmu_partition_support() - Determine if partitioning is possible
 *
 * Partitioning is only supported in VHE mode with PMUv3
 *
 * Return: True if partitioning is possible, false otherwise
 */
bool has_host_pmu_partition_support(void)
{
	return has_vhe() &&
		system_supports_pmuv3();
}

/**
 * pmu_is_partitioned() - Determine if given PMU is partitioned
 * @pmu: Pointer to arm_pmu struct
 *
 * Determine if given PMU is partitioned by looking at hpmn field. The
 * PMU is partitioned if this field is less than the number of
 * counters in the system.
 *
 * Return: True if the PMU is partitioned, false otherwise
 */
bool pmu_is_partitioned(struct arm_pmu *pmu)
{
	if (!pmu)
		return false;

	return pmu->max_guest_counters >= 0 &&
		pmu->max_guest_counters <= *host_data_ptr(nr_event_counters);
}

/**
 * kvm_pmu_is_partitioned() - Determine if KVM has a partitioned PMU
 * @kvm: Pointer to kvm struct
 *
 * Determine if KVM has a partitioned PMU by extracting that field and
 * passing it to :c:func:`pmu_is_partitioned`
 *
 * Return: True if the KVM PMU is partitioned, false otherwise
 */
bool kvm_pmu_is_partitioned(struct kvm *kvm)
{
	return pmu_is_partitioned(kvm->arch.arm_pmu) &&
		test_bit(KVM_ARCH_FLAG_PARTITION_PMU_ENABLED, &kvm->arch.flags);
}

void kvm_pmu_direct_pmcr_write(struct kvm_vcpu *vcpu, u64 val)
{
	bool reset_p = val & ARMV8_PMU_PMCR_P;
	unsigned long mask;
	int i;

	val &= ~ARMV8_PMU_PMCR_P;

	write_sysreg(val, pmcr_el0);

	if (reset_p) {
		mask = kvm_pmu_implemented_counter_mask(vcpu) & ~BIT(ARMV8_PMU_CYCLE_IDX);

		if (!vcpu_is_el2(vcpu))
			mask &= ~kvm_pmu_hyp_counter_mask(vcpu);

		for_each_set_bit(i, &mask, ARMV8_PMU_MAX_GENERAL_COUNTERS)
			write_pmevcntrn(i, 0);
	}
}

u64 kvm_pmu_direct_pmcr_read(struct kvm_vcpu *vcpu)
{
	return u64_replace_bits(
		read_sysreg(pmcr_el0),
		vcpu->kvm->arch.nr_pmu_counters,
		ARMV8_PMU_PMCR_N);
}
