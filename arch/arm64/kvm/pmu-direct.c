// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/perf/arm_pmu.h>
#include <linux/perf/arm_pmuv3.h>

#include <asm/arm_pmuv3.h>

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

	return pmu->max_guest_counters >= 0 &&
		pmu->max_guest_counters <= *host_data_ptr(nr_event_counters);
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

/**
 * kvm_pmu_hpmn() - Calculate HPMN field value
 * @vcpu: Pointer to struct kvm_vcpu
 *
 * Calculate the appropriate value to set for MDCR_EL2.HPMN. If
 * partitioned, this is the number of counters set for the guest if
 * supported, falling back to max_guest_counters if needed. If we are not
 * partitioned or can't set the implied HPMN value, fall back to the
 * host value.
 *
 * Return: A valid HPMN value
 */
u8 kvm_pmu_hpmn(struct kvm_vcpu *vcpu)
{
	u8 nr_guest_cntr = vcpu->kvm->arch.nr_pmu_counters;

	if (kvm_vcpu_pmu_is_partitioned(vcpu)
	    && !vcpu_on_unsupported_cpu(vcpu)
	    && (cpus_have_final_cap(ARM64_HAS_HPMN0) || nr_guest_cntr > 0))
		return nr_guest_cntr;

	return *host_data_ptr(nr_event_counters);
}
