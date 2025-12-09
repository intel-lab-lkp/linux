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
