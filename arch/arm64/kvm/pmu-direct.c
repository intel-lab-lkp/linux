// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/cleanup.h>
#include <linux/kvm_host.h>
#include <linux/lockdep.h>
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
static bool has_host_pmu_partition_support(void)
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

static bool kvm_pmu_part_on_cpu(struct kvm_vcpu *vcpu)
{
	struct arm_pmu *pmu = vcpu->kvm->arch.arm_pmu;

	lockdep_assert_preemption_disabled();

	return kvm_pmu_is_partitioned(vcpu->kvm) &&
	       vcpu == kvm_get_running_vcpu() && pmu &&
	       cpumask_test_cpu(smp_processor_id(), &pmu->supported_cpus);
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

static void kvm_pmu_direct_pmcr_write(struct kvm_vcpu *vcpu, u64 val)
{
	bool reset_p = val & ARMV8_PMU_PMCR_P;
	bool reset_c = val & ARMV8_PMU_PMCR_C;
	unsigned long mask;
	u64 pmcr_mask;
	int i;

	lockdep_assert_preemption_disabled();

	if (!kvm_has_feat(vcpu->kvm, ID_AA64DFR0_EL1, PMUVer, V3P5))
		val &= ~ARMV8_PMU_PMCR_LP;

	pmcr_mask = ARMV8_PMU_PMCR_MASK &
		    ~(ARMV8_PMU_PMCR_P | ARMV8_PMU_PMCR_C);
	__vcpu_rmw_sys_reg(vcpu, PMCR_EL0, &=, ~ARMV8_PMU_PMCR_MASK);
	__vcpu_rmw_sys_reg(vcpu, PMCR_EL0, |=, val & pmcr_mask);

	if (reset_c)
		__vcpu_assign_sys_reg(vcpu, PMCCNTR_EL0, 0);

	mask = kvm_pmu_accessible_counter_mask(vcpu) &
	       ~BIT(ARMV8_PMU_CYCLE_IDX);
	if (reset_p) {
		for_each_set_bit(i, &mask, ARMV8_PMU_MAX_GENERAL_COUNTERS)
			__vcpu_assign_sys_reg(vcpu, PMEVCNTR0_EL0 + i, 0);
	}

	if (!kvm_pmu_part_on_cpu(vcpu))
		return;

	if (reset_c)
		write_pmccntr(0);

	if (reset_p) {
		for_each_set_bit(i, &mask, ARMV8_PMU_MAX_GENERAL_COUNTERS)
			write_pmevcntrn(i, 0);
	}

	write_sysreg(val & ~(ARMV8_PMU_PMCR_P | ARMV8_PMU_PMCR_C), pmcr_el0);
}

static u64 kvm_pmu_direct_pmcr_read(struct kvm_vcpu *vcpu)
{
	u64 n = vcpu->kvm->arch.nr_pmu_counters;
	u64 pmcr;

	lockdep_assert_preemption_disabled();

	if (!kvm_pmu_part_on_cpu(vcpu))
		return kvm_vcpu_read_pmcr(vcpu);

	pmcr = read_sysreg(pmcr_el0);
	if (!kvm_has_feat(vcpu->kvm, ID_AA64DFR0_EL1, PMUVer, V3P5))
		pmcr &= ~ARMV8_PMU_PMCR_LP;

	if (vcpu_has_nv(vcpu) && !vcpu_is_el2(vcpu))
		n = FIELD_GET(MDCR_EL2_HPMN, __vcpu_sys_reg(vcpu, MDCR_EL2));

	return u64_replace_bits(pmcr, n, ARMV8_PMU_PMCR_N);
}

static u64 kvm_pmu_direct_cntr_read(struct kvm_vcpu *vcpu, u8 idx)
{
	lockdep_assert_preemption_disabled();

	if (!kvm_pmu_part_on_cpu(vcpu))
		return __vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + idx);

	if (idx == ARMV8_PMU_CYCLE_IDX)
		return read_pmccntr();

	return read_pmevcntrn(idx);
}

static void kvm_pmu_direct_cntr_write(struct kvm_vcpu *vcpu, u8 idx, u64 val)
{
	lockdep_assert_preemption_disabled();

	__vcpu_assign_sys_reg(vcpu, PMEVCNTR0_EL0 + idx, val);
	if (!kvm_pmu_part_on_cpu(vcpu))
		return;

	if (idx == ARMV8_PMU_CYCLE_IDX)
		write_pmccntr(val);
	else
		write_pmevcntrn(idx, val);
}

/**
 * kvm_pmu_direct_reg_read() - Read a guest PMU register for Partitioned PMU
 * @vcpu: Pointer to vcpu struct
 * @reg: vCPU system register index
 *
 * Return: Register value read from hardware or shadow state
 */
u64 kvm_pmu_direct_reg_read(struct kvm_vcpu *vcpu, unsigned int reg)
{
	unsigned long flags;
	u64 val = 0;

	guard(preempt)();

	switch (reg) {
	case PMCR_EL0:
		return kvm_pmu_direct_pmcr_read(vcpu);
	case PMSELR_EL0:
		if (kvm_pmu_part_on_cpu(vcpu) &&
		    !(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM))
			return read_sysreg(pmselr_el0);
		return __vcpu_sys_reg(vcpu, reg);
	case PMEVCNTR0_EL0 ... PMCCNTR_EL0:
		return kvm_pmu_direct_cntr_read(vcpu, reg - PMEVCNTR0_EL0);
	case PMEVTYPER0_EL0 ... PMCCFILTR_EL0:
		return __vcpu_sys_reg(vcpu, reg);
	case PMCNTENSET_EL0:
		if (kvm_pmu_part_on_cpu(vcpu))
			return read_sysreg(pmcntenset_el0) &
			       kvm_vcpu_pmu_guest_counter_mask(vcpu);
		return __vcpu_sys_reg(vcpu, reg);
	case PMINTENSET_EL1:
		if (kvm_pmu_part_on_cpu(vcpu))
			return read_sysreg(pmintenset_el1) &
			       kvm_vcpu_pmu_guest_counter_mask(vcpu);
		return __vcpu_sys_reg(vcpu, reg);
	case PMOVSSET_EL0:
		local_irq_save(flags);
		if (kvm_pmu_part_on_cpu(vcpu)) {
			u64 hw_ovf = read_sysreg(pmovsset_el0) &
				     kvm_vcpu_pmu_guest_counter_mask(vcpu);

			if (hw_ovf) {
				__vcpu_rmw_sys_reg(vcpu, PMOVSSET_EL0, |=, hw_ovf);
				write_sysreg(hw_ovf, pmovsclr_el0);
			}
		}
		val = __vcpu_sys_reg(vcpu, reg);
		local_irq_restore(flags);
		return val;
	case PMUSERENR_EL0:
		if (kvm_pmu_part_on_cpu(vcpu) &&
		    !(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM))
			return read_sysreg(pmuserenr_el0);
		return __vcpu_sys_reg(vcpu, reg);
	default:
		WARN_ON(1);
		return 0;
	}
}

static void kvm_pmu_apply_single_event_filter(struct kvm_vcpu *vcpu, u8 idx)
{
	u64 guest_counters;
	u64 evtyper_set = ARMV8_PMU_EXCLUDE_EL0 |
		ARMV8_PMU_EXCLUDE_EL1;
	u64 evtyper_clr = ARMV8_PMU_INCLUDE_EL2;
	bool guest_include_el2;
	u64 val;
	u64 evsel;

	guard(preempt)();

	if (!kvm_pmu_part_on_cpu(vcpu))
		return;

	guest_counters = kvm_vcpu_pmu_guest_counter_mask(vcpu);
	if (!test_bit(idx, (unsigned long *)&guest_counters))
		return;

	if (idx == ARMV8_PMU_CYCLE_IDX) {
		val = __vcpu_sys_reg(vcpu, PMCCFILTR_EL0);
		val &= kvm_pmu_evtyper_mask(vcpu->kvm) &
		       ~kvm_pmu_event_mask(vcpu->kvm);
		evsel = ARMV8_PMUV3_PERFCTR_CPU_CYCLES;
	} else {
		val = __vcpu_sys_reg(vcpu, PMEVTYPER0_EL0 + idx);
		val &= kvm_pmu_evtyper_mask(vcpu->kvm);
		evsel = val & kvm_pmu_event_mask(vcpu->kvm);
	}

	guest_include_el2 = (val & ARMV8_PMU_INCLUDE_EL2);
	val &= ~evtyper_clr;

	if (unlikely(is_hyp_ctxt(vcpu))) {
		u64 mdcr = __vcpu_sys_reg(vcpu, MDCR_EL2);
		bool hpmd = (idx == ARMV8_PMU_CYCLE_IDX) ?
			    (mdcr & MDCR_EL2_HCCD) :
			    (!kvm_pmu_counter_is_hyp(vcpu, idx) &&
			     (mdcr & MDCR_EL2_HPMD));

		val &= ~ARMV8_PMU_EXCLUDE_NS_EL1;
		if (guest_include_el2 && !hpmd)
			val &= ~ARMV8_PMU_EXCLUDE_EL1;
		else
			val |= ARMV8_PMU_EXCLUDE_EL1;
	}

	if (vcpu->kvm->arch.pmu_filter &&
	    evsel != ARMV8_PMUV3_PERFCTR_SW_INCR &&
	    evsel != ARMV8_PMUV3_PERFCTR_CHAIN &&
	    !test_bit(evsel, vcpu->kvm->arch.pmu_filter)) {
		val |= evtyper_set;
		val &= ~(ARMV8_PMU_EXCLUDE_NS_EL0 | ARMV8_PMU_EXCLUDE_NS_EL1);
	}

	if (idx == ARMV8_PMU_CYCLE_IDX)
		write_pmccfiltr(val);
	else
		write_pmevtypern(idx, val);
}

/**
 * kvm_pmu_direct_reg_write() - Write a guest PMU register for Partitioned PMU
 * @vcpu: Pointer to vcpu struct
 * @reg: vCPU system register index
 * @val: Value to write
 * @set: True when setting bits, false when clearing bits in mask registers
 */
void kvm_pmu_direct_reg_write(struct kvm_vcpu *vcpu, unsigned int reg,
			      u64 val, bool set)
{
	unsigned long flags;
	u64 mask;
	int idx;

	guard(preempt)();

	switch (reg) {
	case PMCR_EL0:
		kvm_pmu_direct_pmcr_write(vcpu, val);
		break;
	case PMSELR_EL0:
		if (kvm_pmu_part_on_cpu(vcpu) &&
		    !(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM))
			write_sysreg(val, pmselr_el0);
		__vcpu_assign_sys_reg(vcpu, reg, val);
		break;
	case PMEVCNTR0_EL0 ... PMCCNTR_EL0:
		idx = reg - PMEVCNTR0_EL0;
		if (vcpu_mode_is_32bit(vcpu) && idx != ARMV8_PMU_CYCLE_IDX)
			val = (kvm_pmu_direct_cntr_read(vcpu, idx) &
			       GENMASK_ULL(63, 32)) |
			      lower_32_bits(val);
		kvm_pmu_direct_cntr_write(vcpu, idx, val);
		break;
	case PMEVTYPER0_EL0 ... PMCCFILTR_EL0:
		mask = kvm_pmu_evtyper_mask(vcpu->kvm);
		if (reg == PMCCFILTR_EL0)
			mask &= ~kvm_pmu_event_mask(vcpu->kvm);
		__vcpu_assign_sys_reg(vcpu, reg, val & mask);
		kvm_pmu_apply_single_event_filter(vcpu, reg - PMEVTYPER0_EL0);
		break;
	case PMCNTENSET_EL0:
		val &= kvm_vcpu_pmu_guest_counter_mask(vcpu);
		if (set) {
			__vcpu_rmw_sys_reg(vcpu, PMCNTENSET_EL0, |=, val);
			if (kvm_pmu_part_on_cpu(vcpu))
				write_sysreg(val, pmcntenset_el0);
		} else {
			__vcpu_rmw_sys_reg(vcpu, PMCNTENSET_EL0, &=, ~val);
			if (kvm_pmu_part_on_cpu(vcpu))
				write_sysreg(val, pmcntenclr_el0);
		}
		break;
	case PMINTENSET_EL1:
		val &= kvm_vcpu_pmu_guest_counter_mask(vcpu);
		if (set) {
			__vcpu_rmw_sys_reg(vcpu, PMINTENSET_EL1, |=, val);
			if (kvm_pmu_part_on_cpu(vcpu))
				write_sysreg(val, pmintenset_el1);
		} else {
			__vcpu_rmw_sys_reg(vcpu, PMINTENSET_EL1, &=, ~val);
			if (kvm_pmu_part_on_cpu(vcpu))
				write_sysreg(val, pmintenclr_el1);
		}
		break;
	case PMOVSSET_EL0:
		local_irq_save(flags);
		if (set) {
			__vcpu_rmw_sys_reg(vcpu, PMOVSSET_EL0, |=, val);
		} else {
			mask = kvm_vcpu_pmu_guest_counter_mask(vcpu);
			if (kvm_pmu_part_on_cpu(vcpu))
				write_sysreg(val & mask, pmovsclr_el0);
			__vcpu_rmw_sys_reg(vcpu, PMOVSSET_EL0, &=, ~val);
		}
		local_irq_restore(flags);
		break;
	case PMUSERENR_EL0:
		val &= ARMV8_PMU_USERENR_MASK;
		if (kvm_pmu_part_on_cpu(vcpu) &&
		    !(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM))
			write_sysreg(val, pmuserenr_el0);
		__vcpu_assign_sys_reg(vcpu, reg, val);
		break;
	default:
		WARN_ON(1);
		break;
	}
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

static void kvm_pmu_apply_event_filter(struct kvm_vcpu *vcpu)
{
	struct arm_pmu *pmu = vcpu->kvm->arch.arm_pmu;
	unsigned long guest_counters;
	u8 i;

	if (!pmu)
		return;

	guest_counters = kvm_vcpu_pmu_guest_counter_mask(vcpu);

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS)
		kvm_pmu_apply_single_event_filter(vcpu, i);
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
	struct arm_pmu *pmu = vcpu->kvm->arch.arm_pmu;
	unsigned long guest_counters;
	unsigned long flags;
	u64 mask;
	u8 i;
	u64 val;

	/*
	 * If we aren't guest-owned then we know the guest isn't using
	 * the PMU anyway, so no need to bother with the swap.
	 */
	if (!kvm_pmu_is_partitioned(vcpu->kvm) || !vcpu->wants_to_run || !pmu)
		return;

	local_irq_save(flags);
	if (!cpumask_test_cpu(smp_processor_id(), &pmu->supported_cpus)) {
		local_irq_restore(flags);
		return;
	}

	guest_counters = kvm_vcpu_pmu_guest_counter_mask(vcpu);
	kvm_pmu_apply_event_filter(vcpu);

	for_each_set_bit(i, &guest_counters, ARMPMU_MAX_HWEVENTS) {
		val = __vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + i);

		if (i == ARMV8_PMU_CYCLE_IDX)
			write_pmccntr(val);
		else
			write_pmevcntrn(i, val);
	}

	if (!(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM)) {
		val = __vcpu_sys_reg(vcpu, PMSELR_EL0);
		write_sysreg(val, pmselr_el0);

		val = __vcpu_sys_reg(vcpu, PMUSERENR_EL0) &
		      ARMV8_PMU_USERENR_MASK;
		write_sysreg(val, pmuserenr_el0);
	}

	/* Save only the stateful writable bits. */
	val = __vcpu_sys_reg(vcpu, PMCR_EL0);
	mask = ARMV8_PMU_PMCR_MASK &
		~(ARMV8_PMU_PMCR_P | ARMV8_PMU_PMCR_C);
	if (!kvm_has_feat(vcpu->kvm, ID_AA64DFR0_EL1, PMUVer, V3P5))
		mask &= ~ARMV8_PMU_PMCR_LP;
	write_sysreg(val & mask, pmcr_el0);

	/*
	 * When handling these:
	 * 1. Apply only the bits for guest counters (indicated by mask)
	 * 2. Use the different registers for set and clear
	 */
	mask = guest_counters;

	/*
	 * Clear the hardware overflow flags so there is no chance of
	 * creating spurious interrupts. The hardware here is never
	 * the canonical version anyway.
	 */
	write_sysreg(mask, pmovsclr_el0);
	isb();

	val = __vcpu_sys_reg(vcpu, PMCNTENSET_EL0);
	write_sysreg(val & mask, pmcntenset_el0);
	write_sysreg(~val & mask, pmcntenclr_el0);

	val = __vcpu_sys_reg(vcpu, PMINTENSET_EL1);
	write_sysreg(val & mask, pmintenset_el1);
	write_sysreg(~val & mask, pmintenclr_el1);

	local_irq_restore(flags);
}

/**
 * kvm_pmu_put() - Put untrapped PMU registers
 * @vcpu: Pointer to struct kvm_vcpu
 *
 * Put all untrapped PMU registers from the PCPU into the VCPU. Mask
 * to only bits belonging to guest-reserved counters and leave
 * host-reserved counters alone in bitmask registers.
 */
void kvm_pmu_put(struct kvm_vcpu *vcpu)
{
	struct arm_pmu *pmu = vcpu->kvm->arch.arm_pmu;
	unsigned long guest_counters;
	unsigned long flags;
	u64 pmcr_mask;
	u64 mask;
	u8 i;
	u64 val;

	/*
	 * If we aren't guest-owned then we know the guest is not
	 * accessing the PMU anyway, so no need to bother with the
	 * swap.
	 */
	if (!kvm_pmu_is_partitioned(vcpu->kvm) || !vcpu->wants_to_run || !pmu)
		return;

	local_irq_save(flags);
	if (!cpumask_test_cpu(smp_processor_id(), &pmu->supported_cpus)) {
		local_irq_restore(flags);
		return;
	}

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
		if (i == ARMV8_PMU_CYCLE_IDX) {
			val = read_pmccntr();
			write_pmccntr(0);
		} else {
			val = read_pmevcntrn(i);
			write_pmevcntrn(i, 0);
		}

		__vcpu_assign_sys_reg(vcpu, PMEVCNTR0_EL0 + i, val);
	}

	if (!(vcpu->arch.mdcr_el2 & MDCR_EL2_TPM)) {
		val = read_sysreg(pmselr_el0);
		__vcpu_assign_sys_reg(vcpu, PMSELR_EL0, val);
		write_sysreg(0, pmselr_el0);

		val = read_sysreg(pmuserenr_el0) & ARMV8_PMU_USERENR_MASK;
		__vcpu_assign_sys_reg(vcpu, PMUSERENR_EL0, val);
		write_sysreg(ARMV8_PMU_USERENR_MASK, pmuserenr_el0);
	}

	val = read_sysreg(pmcr_el0);
	pmcr_mask = ARMV8_PMU_PMCR_MASK;
	if (!kvm_has_feat(vcpu->kvm, ID_AA64DFR0_EL1, PMUVer, V3P5))
		pmcr_mask &= ~ARMV8_PMU_PMCR_LP;
	__vcpu_rmw_sys_reg(vcpu, PMCR_EL0, &=, ~ARMV8_PMU_PMCR_MASK);
	__vcpu_rmw_sys_reg(vcpu, PMCR_EL0, |=, val & pmcr_mask);

	val = ARMV8_PMU_PMCR_LC;
	if (pmu->pmuver >= ID_AA64DFR0_EL1_PMUVer_V3P5)
		val |= ARMV8_PMU_PMCR_LP;
	if (vcpu->arch.mdcr_el2 & MDCR_EL2_HPME)
		val |= ARMV8_PMU_PMCR_E;
	write_sysreg(val, pmcr_el0);

	/* Save pending guest hardware overflows. */
	val = read_sysreg(pmovsset_el0);
	__vcpu_rmw_sys_reg(vcpu, PMOVSSET_EL0, |=, val & mask);
	write_sysreg(val & mask, pmovsclr_el0);

	local_irq_restore(flags);
}
