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
		vcpu->arch.pmu.access == VCPU_PMU_ACCESS_GUEST_OWNED &&
		cpus_have_final_cap(ARM64_HAS_FGT) &&
		(hpmn != 0 || cpus_have_final_cap(ARM64_HAS_HPMN0));
}

/**
 * kvm_pmu_set_physical_access()
 * @vcpu: Pointer to vcpu struct
 *
 * Reconfigure the guest for physical access of PMU hardware if
 * allowed. This means reconfiguring mdcr_el2 and loading the vCPU
 * state onto hardware.
 *
 */

void kvm_pmu_set_physical_access(struct kvm_vcpu *vcpu)
{
	if (kvm_vcpu_pmu_is_partitioned(vcpu)
	    && vcpu->arch.pmu.access == VCPU_PMU_ACCESS_FREE) {
		vcpu->arch.pmu.access = VCPU_PMU_ACCESS_GUEST_OWNED;
		kvm_arm_setup_mdcr_el2(vcpu);
	}
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

	return GENMASK(nr_counters - 1, pmu->max_guest_counters);
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
	return ARMV8_PMU_CNT_MASK_C & GENMASK(pmu->max_guest_counters - 1, 0);
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

/**
 * kvm_pmu_apply_event_filter()
 * @vcpu: Pointer to vcpu struct
 *
 * To uphold the guarantee of the KVM PMU event filter, we must ensure
 * no counter counts if the event is filtered. Accomplish this by
 * filtering all exception levels if the event is filtered.
 */
static void kvm_pmu_apply_event_filter(struct kvm_vcpu *vcpu)
{
	struct arm_pmu *pmu = vcpu->kvm->arch.arm_pmu;
	unsigned long guest_counters = kvm_pmu_guest_counter_mask(pmu);
	u64 evtyper_set = ARMV8_PMU_EXCLUDE_EL0 |
		ARMV8_PMU_EXCLUDE_EL1;
	u64 evtyper_clr = ARMV8_PMU_INCLUDE_EL2;
	bool guest_include_el2;
	u8 i;
	u64 val;
	u64 evsel;

	if (!pmu)
		return;

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		if (i == ARMV8_PMU_CYCLE_IDX) {
			val = __vcpu_sys_reg(vcpu, PMCCFILTR_EL0);
			evsel = ARMV8_PMUV3_PERFCTR_CPU_CYCLES;
		} else {
			val = __vcpu_sys_reg(vcpu, PMEVTYPER0_EL0 + i);
			evsel = val & kvm_pmu_event_mask(vcpu->kvm);
		}

		guest_include_el2 = (val & ARMV8_PMU_INCLUDE_EL2);
		val &= ~evtyper_clr;

		if (unlikely(is_hyp_ctxt(vcpu)) && guest_include_el2)
			val &= ~ARMV8_PMU_EXCLUDE_EL1;

		if (vcpu->kvm->arch.pmu_filter &&
		    !test_bit(evsel, vcpu->kvm->arch.pmu_filter))
			val |= evtyper_set;

		write_sysreg(i, pmselr_el0);
		write_sysreg(val, pmxevtyper_el0);
	}
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
	if (!kvm_vcpu_pmu_is_partitioned(vcpu) ||
	    vcpu->arch.pmu.access != VCPU_PMU_ACCESS_GUEST_OWNED)
		return;

	preempt_disable();

	pmu = vcpu->kvm->arch.arm_pmu;
	guest_counters = kvm_pmu_guest_counter_mask(pmu);
	kvm_pmu_apply_event_filter(vcpu);

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		val = __vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + i);

		write_sysreg(i, pmselr_el0);
		write_sysreg(val, pmxevcntr_el0);
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
	u64 mask;
	u8 i;
	u64 val;

	/*
	 * If we aren't guest-owned then we know the guest is not
	 * accessing the PMU anyway, so no need to bother with the
	 * swap.
	 */
	if (!kvm_vcpu_pmu_is_partitioned(vcpu) ||
	    vcpu->arch.pmu.access != VCPU_PMU_ACCESS_GUEST_OWNED)
		return;

	preempt_disable();

	pmu = vcpu->kvm->arch.arm_pmu;
	guest_counters = kvm_pmu_guest_counter_mask(pmu);

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		write_sysreg(i, pmselr_el0);
		val = read_sysreg(pmxevcntr_el0);

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

	preempt_enable();
}
