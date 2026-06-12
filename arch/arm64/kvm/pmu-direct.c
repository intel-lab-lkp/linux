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

/* Callback to update counter mask between perf scheduling */
static void kvm_pmu_update_mask(struct pmu *pmu, void *data)
{
	struct arm_pmu *arm_pmu = to_arm_pmu(pmu);
	unsigned long *new_mask = data;

	bitmap_copy(arm_pmu->cntr_mask, new_mask, ARMPMU_MAX_HWEVENTS);
}

/**
 * kvm_pmu_set_guest_counters() - Handle dynamic counter reservations
 * @cpu_pmu: struct arm_pmu to potentially modify
 * @guest_mask: new guest mask for the pmu
 *
 * Check if guest counters will interfere with current host events and
 * call into perf_pmu_resched_update if a reschedule is required.
 */
static void kvm_pmu_set_guest_counters(struct arm_pmu *cpu_pmu, u64 guest_mask)
{
	struct pmu_hw_events *cpuc = this_cpu_ptr(cpu_pmu->hw_events);
	DECLARE_BITMAP(guest_bitmap, ARMPMU_MAX_HWEVENTS);
	DECLARE_BITMAP(new_mask, ARMPMU_MAX_HWEVENTS);
	bool need_resched = false;

	bitmap_from_arr64(guest_bitmap, &guest_mask, ARMPMU_MAX_HWEVENTS);
	bitmap_copy(new_mask, cpu_pmu->hw_cntr_impl, ARMPMU_MAX_HWEVENTS);

	if (guest_mask) {
		/* Subtract guest counters from available host mask */
		bitmap_andnot(new_mask, new_mask, guest_bitmap, ARMPMU_MAX_HWEVENTS);

		/* Did we collide with an active host event? */
		if (bitmap_intersects(cpuc->used_mask, guest_bitmap, ARMPMU_MAX_HWEVENTS)) {
			int idx;

			need_resched = true;
			cpuc->host_squeezed = true;

			/* Look for pinned events that are about to be preempted */
			for_each_set_bit(idx, guest_bitmap, ARMPMU_MAX_HWEVENTS) {
				if (test_bit(idx, cpuc->used_mask) && cpuc->events[idx] &&
				    cpuc->events[idx]->attr.pinned) {
					pr_warn_once("perf: Pinned host event squeezed out by KVM guest PMU partition\n");
					break;
				}
			}
		}
	} else {
		/*
		 * Restoring to hw_cntr_impl.
		 * Only resched if we previously squeezed an event.
		 */
		if (cpuc->host_squeezed) {
			need_resched = true;
			cpuc->host_squeezed = false;
		}
	}

	if (need_resched) {
		/* Collision: run full perf reschedule */
		perf_pmu_resched_update(&cpu_pmu->pmu, kvm_pmu_update_mask, new_mask);
	} else {
		/* Host was never using guest counters anyway */
		bitmap_copy(cpu_pmu->cntr_mask, new_mask, ARMPMU_MAX_HWEVENTS);
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
	unsigned long guest_counters;
	u64 evtyper_set = ARMV8_PMU_EXCLUDE_EL0 |
		ARMV8_PMU_EXCLUDE_EL1;
	u64 evtyper_clr = ARMV8_PMU_INCLUDE_EL2;
	bool guest_include_el2;
	u8 i;
	u64 val;
	u64 evsel;

	if (!pmu)
		return;

	guest_counters = kvm_pmu_guest_counter_mask(pmu);

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

		if (i == ARMV8_PMU_CYCLE_IDX)
			write_pmccfiltr(val);
		else
			write_pmevtypern(i, val);
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
	if (vcpu->arch.pmu.access != VCPU_PMU_ACCESS_GUEST_OWNED)
		return;

	preempt_disable();

	pmu = vcpu->kvm->arch.arm_pmu;
	guest_counters = kvm_pmu_guest_counter_mask(pmu);
	kvm_pmu_set_guest_counters(pmu, guest_counters);
	kvm_pmu_apply_event_filter(vcpu);

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
	if (vcpu->arch.pmu.access != VCPU_PMU_ACCESS_GUEST_OWNED)
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

/**
 * kvm_pmu_set_guest_owned() - Give PMU ownership to guest
 * @vcpu: Pointer to vcpu struct
 *
 * Reconfigure the guest for physical access of PMU hardware if
 * allowed. This means reconfiguring mdcr_el2.
 *
 */
void kvm_pmu_set_guest_owned(struct kvm_vcpu *vcpu)
{
	if (kvm_pmu_is_partitioned(vcpu->kvm) &&
	    vcpu->arch.pmu.access == VCPU_PMU_ACCESS_FREE) {
		vcpu->arch.pmu.access = VCPU_PMU_ACCESS_GUEST_OWNED;
		kvm_arm_setup_mdcr_el2(vcpu);
	}
}
