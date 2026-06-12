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
