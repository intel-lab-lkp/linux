// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/perf/arm_pmu.h>
#include <linux/perf/arm_pmuv3.h>

#include <asm/kvm_pmu.h>
#include <asm/arm_pmuv3.h>

/**
 * kvm_pmu_reservation_is_valid() - Determine if reservation is allowed
 * @host_counters: Number of host counters to reserve
 *
 * Determine if the number of host counters in the argument is
 * allowed. It is allowed if it will produce a valid value for
 * register field MDCR_EL2.HPMN.
 *
 * Return: True if reservation allowed, false otherwise
 */
static bool kvm_pmu_reservation_is_valid(u8 host_counters)
{
	u8 nr_counters = *host_data_ptr(nr_event_counters);

	return host_counters < nr_counters ||
		(host_counters == nr_counters
		 && cpus_have_final_cap(ARM64_HAS_HPMN0));
}

/**
 * kvm_pmu_hpmn() - Compute HPMN value
 * @host_counters: Number of host counters to reserve
 *
 * This function computes the value of HPMN, the partition pivot
 * value, such that counters 0..HPMN are reserved for the guest and
 * counters HPMN..N are reserved for the host.
 *
 * If the requested @host_counters would create an invalid partition,
 * return the value of HPMN that creates no partition.
 *
 * Return: Value of HPMN
 */
u8 kvm_pmu_hpmn(u8 host_counters)
{
	u8 nr_counters = *host_data_ptr(nr_event_counters);

	if (likely(kvm_pmu_reservation_is_valid(host_counters)))
		return nr_counters - host_counters;
	else
		return nr_counters;
}

/**
 * kvm_pmu_partition_supported() - Determine if partitioning is possible
 *
 * Partitioning is only supported in VHE mode where we have PMUv3 and
 * Fine Grain Traps (FGT).
 *
 * Return: True if partitioning is possible, false otherwise
 */
bool kvm_pmu_partition_supported(void)
{
	return has_vhe()
		&& pmuv3_implemented(kvm_arm_pmu_get_pmuver_limit())
		&& cpus_have_final_cap(ARM64_HAS_FGT);
}

/**
 * kvm_pmu_partition() - Partition the PMU
 * @pmu: Pointer to pmu being partitioned
 * @host_counters: Number of host counters to reserve
 *
 * Partition the given PMU by taking a number of host counters to
 * reserve and, if it is a valid reservation, recording the
 * corresponding HPMN value in the hpmn field of the PMU and clearing
 * the guest-reserved counters from the counter mask.
 *
 * Passing 0 for @host_counters has the effect of disabling partitioning.
 *
 * Return: 0 on success, -ERROR otherwise
 */
int kvm_pmu_partition(struct arm_pmu *pmu, u8 host_counters)
{
	u8 nr_counters;
	u8 hpmn;

	if (!kvm_pmu_reservation_is_valid(host_counters))
		return -EINVAL;

	nr_counters = *host_data_ptr(nr_event_counters);
	hpmn = kvm_pmu_hpmn(host_counters);

	if (hpmn < nr_counters) {
		pmu->hpmn = hpmn;
		/* Inform host driver of available counters */
		bitmap_clear(pmu->cntr_mask, 0, hpmn);
		bitmap_set(pmu->cntr_mask, hpmn, nr_counters);
		clear_bit(ARMV8_PMU_CYCLE_IDX, pmu->cntr_mask);
		if (pmuv3_has_icntr())
			clear_bit(ARMV8_PMU_INSTR_IDX, pmu->cntr_mask);

		kvm_debug("Partitioned PMU with HPMN %u", hpmn);
	} else {
		pmu->hpmn = nr_counters;
		bitmap_set(pmu->cntr_mask, 0, nr_counters);
		set_bit(ARMV8_PMU_CYCLE_IDX, pmu->cntr_mask);
		if (pmuv3_has_icntr())
			set_bit(ARMV8_PMU_INSTR_IDX, pmu->cntr_mask);

		kvm_debug("Unpartitioned PMU");
	}

	return 0;
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
	return pmu->hpmn < *host_data_ptr(nr_event_counters);
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
	return kvm_pmu_is_partitioned(vcpu->kvm->arch.arm_pmu);
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

	return GENMASK(nr_counters - 1, pmu->hpmn);
}

/** kvm_pmu_guest_counter_mask() - Compute bitmask of guest-reserved counters
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

/** kvm_pmu_host_counters_enable() - Enable host-reserved counters
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

/** kvm_pmu_host_counters_disable() - Disable host-reserved counters
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
