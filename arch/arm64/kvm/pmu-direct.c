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

	if (pmu_is_partitioned(pmu))
		return GENMASK_ULL(nr_counters - 1, pmu->max_guest_counters);

	return ARMV8_PMU_CNT_MASK_ALL;
}

/**
 * kvm_pmu_guest_counter_mask() - Compute bitmask of guest-reserved counters
 * @pmu: Pointer to arm_pmu struct
 *
 * Compute the bitmask that selects the guest-reserved counters in the
 * {PMCNTEN,PMINTEN,PMOVS}{SET,CLR} registers. These are the counters
 * in 0..HPMN and the cycle and instruction counters.
 *
 * Return: Bitmask
 */
u64 kvm_pmu_guest_counter_mask(struct arm_pmu *pmu)
{
	if (pmu_is_partitioned(pmu)) {
		u64 mask = ARMV8_PMU_CNT_MASK_C;

		if (pmu->max_guest_counters > 0)
			mask |= GENMASK_ULL(pmu->max_guest_counters - 1, 0);

		return mask;
	}

	return 0;
}

/**
 * kvm_pmu_load() - Load untrapped PMU registers
 * @vcpu: Pointer to struct kvm_vcpu
 *
 * Load all untrapped PMU registers from the VCPU into the PCPU. Mask
 * to only bits belonging to guest-reserved counters and leave
 * host-reserved counters alone in bitmask registers.
 */
void kvm_pmu_load(struct kvm_vcpu *vcpu)
{
	struct arm_pmu *pmu;
	unsigned long guest_counters;
	u64 mask;
	u8 i;
	u64 val;

	/*
	 * If we aren't guest-owned then we know the guest isn't using
	 * the PMU anyway, so no need to bother with the swap.
	 */
	if (!kvm_pmu_is_partitioned(vcpu->kvm))
		return;

	preempt_disable();

	pmu = vcpu->kvm->arch.arm_pmu;
	guest_counters = kvm_pmu_guest_counter_mask(pmu);

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		val = __vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + i);

		if (i == ARMV8_PMU_CYCLE_IDX)
			write_pmccntr(val);
		else
			write_pmevcntrn(i, val);
	}

	val = __vcpu_sys_reg(vcpu, PMSELR_EL0);
	write_sysreg(val, pmselr_el0);

	/* Save only the stateful writable bits. */
	val = __vcpu_sys_reg(vcpu, PMCR_EL0);
	mask = ARMV8_PMU_PMCR_MASK &
		~(ARMV8_PMU_PMCR_P | ARMV8_PMU_PMCR_C);
	write_sysreg(val & mask, pmcr_el0);

	/*
	 * When handling these:
	 * 1. Apply only the bits for guest counters (indicated by mask)
	 * 2. Use the different registers for set and clear
	 */
	mask = kvm_pmu_guest_counter_mask(pmu);

	/* Clear the hardware overflow flags so there is no chance of
	 * creating spurious interrupts. The hardware here is never
	 * the canonical version anyway.
	 */
	write_sysreg(mask, pmovsclr_el0);

	val = __vcpu_sys_reg(vcpu, PMCNTENSET_EL0);
	write_sysreg(val & mask, pmcntenset_el0);
	write_sysreg(~val & mask, pmcntenclr_el0);

	val = __vcpu_sys_reg(vcpu, PMINTENSET_EL1);
	write_sysreg(val & mask, pmintenset_el1);
	write_sysreg(~val & mask, pmintenclr_el1);

	preempt_enable();
}

/**
 * kvm_pmu_put() - Put untrapped PMU registers
 * @vcpu: Pointer to struct kvm_vcpu
 *
 * Put all untrapped PMU registers from the VCPU into the PCPU. Mask
 * to only bits belonging to guest-reserved counters and leave
 * host-reserved counters alone in bitmask registers.
 */
void kvm_pmu_put(struct kvm_vcpu *vcpu)
{
	struct arm_pmu *pmu;
	unsigned long guest_counters;
	unsigned long flags;
	u64 mask;
	u8 i;
	u64 val;

	/*
	 * If we aren't guest-owned then we know the guest is not
	 * accessing the PMU anyway, so no need to bother with the
	 * swap.
	 */
	if (!kvm_pmu_is_partitioned(vcpu->kvm))
		return;

	preempt_disable();

	pmu = vcpu->kvm->arch.arm_pmu;
	guest_counters = kvm_pmu_guest_counter_mask(pmu);

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		if (i == ARMV8_PMU_CYCLE_IDX)
			val = read_pmccntr();
		else
			val = read_pmevcntrn(i);

		__vcpu_assign_sys_reg(vcpu, PMEVCNTR0_EL0 + i, val);
	}

	val = read_sysreg(pmselr_el0);
	__vcpu_assign_sys_reg(vcpu, PMSELR_EL0, val);

	val = read_sysreg(pmcr_el0);
	__vcpu_assign_sys_reg(vcpu, PMCR_EL0, val);

	/* Mask these to only save the guest relevant bits. */
	mask = kvm_pmu_guest_counter_mask(pmu);

	val = read_sysreg(pmcntenset_el0);
	__vcpu_assign_sys_reg(vcpu, PMCNTENSET_EL0, val & mask);

	val = read_sysreg(pmintenset_el1);
	__vcpu_assign_sys_reg(vcpu, PMINTENSET_EL1, val & mask);

	/* Save pending guest hardware overflows. */
	local_irq_save(flags);
	val = read_sysreg(pmovsset_el0);
	__vcpu_rmw_sys_reg(vcpu, PMOVSSET_EL0, |=, val & mask);
	write_sysreg(val & mask, pmovsclr_el0);
	local_irq_restore(flags);

	/* Stop guest counters and disable interrupts in hardware. */
	write_sysreg(mask, pmcntenclr_el0);
	write_sysreg(mask, pmintenclr_el1);

	kvm_pmu_set_guest_counters(pmu, 0);
	preempt_enable();
}
