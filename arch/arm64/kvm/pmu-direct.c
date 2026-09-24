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
#include <asm/virt.h>

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
 * has_kvm_pmu_partition_support() - If we can enable/disable partition
 *
 * Return: true if allowed, false otherwise.
 */
bool has_kvm_pmu_partition_support(void)
{
	return has_host_pmu_partition_support() &&
		kvm_supports_guest_pmuv3();
}

/**
 * kvm_pmu_partition_enable() - Enable/disable partition flag
 * @kvm: Pointer to kvm struct
 * @enable: Whether to enable or disable
 *
 * If we want to enable the partition, the guest is free to grab
 * hardware by accessing PMU registers. Otherwise, the host maintains
 * control.
 */
void kvm_pmu_partition_enable(struct kvm *kvm, bool enable)
{
	if (enable)
		set_bit(KVM_ARCH_FLAG_PARTITION_PMU_ENABLED, &kvm->arch.flags);
	else
		clear_bit(KVM_ARCH_FLAG_PARTITION_PMU_ENABLED, &kvm->arch.flags);
}

/**
 * kvm_pmu_is_partitioned() - Determine if KVM has a partitioned PMU
 * @kvm: Pointer to kvm struct
 *
 * Return: True if the KVM PMU is partitioned, false otherwise
 */
bool kvm_pmu_is_partitioned(struct kvm *kvm)
{
	return kvm && test_bit(KVM_ARCH_FLAG_PARTITION_PMU_ENABLED, &kvm->arch.flags);
}

/**
 * kvm_pmu_direct_pmcr_write() - Handle guest writes to PMCR_EL0
 * @vcpu: Pointer to vcpu struct
 * @val: Value written to PMCR_EL0
 *
 * Write control bits to physical pmcr_el0 and reset guest-owned general
 * event counters when PMCR_EL0.P is set.
 */
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

/**
 * kvm_pmu_direct_pmcr_read() - Handle guest reads from PMCR_EL0
 * @vcpu: Pointer to vcpu struct
 *
 * Read physical pmcr_el0 and replace PMCR_EL0.N with the number of
 * event counters partitioned to the guest.
 *
 * Return: Filtered PMCR_EL0 value
 */
u64 kvm_pmu_direct_pmcr_read(struct kvm_vcpu *vcpu)
{
	u64 n = vcpu->kvm->arch.nr_pmu_counters;

	if (vcpu_has_nv(vcpu) && !vcpu_is_el2(vcpu))
		n = FIELD_GET(MDCR_EL2_HPMN, __vcpu_sys_reg(vcpu, MDCR_EL2));

	return u64_replace_bits(
		read_sysreg(pmcr_el0),
		n,
		ARMV8_PMU_PMCR_N);
}

/**
 * kvm_pmu_host_counter_mask() - Compute bitmask of host-reserved counters
 *
 * Compute the bitmask that selects the host-reserved counters in the
 * {PMCNTEN,PMINTEN,PMOVS}{SET,CLR} registers. These are the counters
 * in HPMN..N-1.
 *
 * Return: Bitmask
 */
u64 kvm_pmu_host_counter_mask(void)
{
	struct kvm_vcpu *vcpu = kvm_get_running_vcpu();

	if (vcpu && kvm_pmu_is_partitioned(vcpu->kvm)) {
		u8 nr_counters = *host_data_ptr(nr_event_counters);
		unsigned int guest_counters = vcpu->kvm->arch.nr_pmu_counters;

		if (nr_counters > guest_counters)
			return GENMASK_ULL(nr_counters - 1, guest_counters);
		else
			return 0;
	}

	return ARMV8_PMU_CNT_MASK_ALL;
}

static u64 kvm_vcpu_pmu_guest_counter_mask(struct kvm_vcpu *vcpu)
{
	if (vcpu && kvm_pmu_is_partitioned(vcpu->kvm)) {
		u64 mask = ARMV8_PMU_CNT_MASK_C;
		unsigned int guest_counters = vcpu->kvm->arch.nr_pmu_counters;

		if (guest_counters > 0)
			mask |= GENMASK_ULL(guest_counters - 1, 0);

		return mask;
	}

	return 0;
}
/**
 * kvm_pmu_guest_counter_mask() - Compute bitmask of guest-reserved counters
 *
 * Compute the bitmask that selects the guest-reserved counters in the
 * {PMCNTEN,PMINTEN,PMOVS}{SET,CLR} registers. These are the event counters
 * in 0..HPMN-1 and the cycle counter (PMCCNTR_EL0).
 *
 * Return: Bitmask
 */
u64 kvm_pmu_guest_counter_mask(void)
{
	return kvm_vcpu_pmu_guest_counter_mask(kvm_get_running_vcpu());
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

	guest_counters = kvm_vcpu_pmu_guest_counter_mask(vcpu);

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		val = __vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + i);

		if (i == ARMV8_PMU_CYCLE_IDX)
			write_pmccntr(val);
		else
			write_pmevcntrn(i, val);
	}

	val = __vcpu_sys_reg(vcpu, PMSELR_EL0);
	write_sysreg(val, pmselr_el0);

	if (!(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM)) {
		val = __vcpu_sys_reg(vcpu, PMUSERENR_EL0);
		write_sysreg(val, pmuserenr_el0);
	}

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
	mask = guest_counters;

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

	guest_counters = kvm_vcpu_pmu_guest_counter_mask(vcpu);
	mask = guest_counters;

	/* Mask these to only save the guest relevant bits. */
	val = read_sysreg(pmcntenset_el0);
	__vcpu_assign_sys_reg(vcpu, PMCNTENSET_EL0, val & mask);

	val = read_sysreg(pmintenset_el1);
	__vcpu_assign_sys_reg(vcpu, PMINTENSET_EL1, val & mask);

	/* Stop guest counters and disable interrupts in hardware first. */
	write_sysreg(mask, pmcntenclr_el0);
	write_sysreg(mask, pmintenclr_el1);
	isb();

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		if (i == ARMV8_PMU_CYCLE_IDX)
			val = read_pmccntr();
		else
			val = read_pmevcntrn(i);

		__vcpu_assign_sys_reg(vcpu, PMEVCNTR0_EL0 + i, val);
	}

	val = read_sysreg(pmselr_el0);
	__vcpu_assign_sys_reg(vcpu, PMSELR_EL0, val);

	if (!(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM)) {
		val = read_sysreg(pmuserenr_el0);
		__vcpu_assign_sys_reg(vcpu, PMUSERENR_EL0, val);
	}

	val = read_sysreg(pmcr_el0);
	__vcpu_rmw_sys_reg(vcpu, PMCR_EL0, &=, ~ARMV8_PMU_PMCR_MASK);
	__vcpu_rmw_sys_reg(vcpu, PMCR_EL0, |=, val & ARMV8_PMU_PMCR_MASK);

	val = ARMV8_PMU_PMCR_LC;
	if (pmu && pmu->pmuver >= ID_AA64DFR0_EL1_PMUVer_V3P5)
		val |= ARMV8_PMU_PMCR_LP;
	if (vcpu->arch.mdcr_el2 & MDCR_EL2_HPME)
		val |= ARMV8_PMU_PMCR_E;
	write_sysreg(val, pmcr_el0);

	/* Save pending guest hardware overflows. */
	local_irq_save(flags);
	val = read_sysreg(pmovsset_el0);
	__vcpu_rmw_sys_reg(vcpu, PMOVSSET_EL0, |=, val & mask);
	write_sysreg(val & mask, pmovsclr_el0);
	local_irq_restore(flags);

	preempt_enable();
}
