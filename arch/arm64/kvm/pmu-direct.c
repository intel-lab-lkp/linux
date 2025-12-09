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
		vcpu->arch.pmu.access == VCPU_PMU_ACCESS_PHYSICAL &&
		cpus_have_final_cap(ARM64_HAS_FGT) &&
		(hpmn != 0 || cpus_have_final_cap(ARM64_HAS_HPMN0));
}

u64 kvm_pmu_fgt_bits(void)
{
	return HDFGRTR_EL2_PMOVS
		| HDFGRTR_EL2_PMCCFILTR_EL0
		| HDFGRTR_EL2_PMEVTYPERn_EL0
		| HDFGRTR_EL2_PMCEIDn_EL0
		| HDFGRTR_EL2_PMMIR_EL1;
}

u64 kvm_pmu_fgt2_bits(void)
{
	return HDFGRTR2_EL2_nPMICFILTR_EL0
		| HDFGRTR2_EL2_nPMICNTR_EL0;
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
	    && vcpu->arch.pmu.access == VCPU_PMU_ACCESS_VIRTUAL) {
		vcpu->arch.pmu.access = VCPU_PMU_ACCESS_PHYSICAL;
		kvm_arm_setup_mdcr_el2(vcpu);
		kvm_pmu_load(vcpu);
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

/**
 * kvm_pmu_guest_num_counters() - Number of counters to show to guest
 * @vcpu: Pointer to struct kvm_vcpu
 *
 * Calculate the number of counters to show to the guest via
 * PMCR_EL0.N, making sure to respect the maximum the host allows,
 * which is hpmn_max if partitioned and host_max otherwise.
 *
 * Return: Valid value for PMCR_EL0.N
 */
u8 kvm_pmu_guest_num_counters(struct kvm_vcpu *vcpu)
{
	u8 nr_cnt = vcpu->kvm->arch.nr_pmu_counters;
	int hpmn_max = armv8pmu_hpmn_max;
	u8 host_max = *host_data_ptr(nr_event_counters);

	if (vcpu->kvm->arch.arm_pmu)
		hpmn_max = vcpu->kvm->arch.arm_pmu->hpmn_max;

	if (kvm_vcpu_pmu_is_partitioned(vcpu)) {
		if (nr_cnt <= hpmn_max && nr_cnt <= host_max)
			return nr_cnt;
		if (hpmn_max <= host_max)
			return hpmn_max;
	}

	if (nr_cnt <= host_max)
		return nr_cnt;

	return host_max;
}

/**
 * kvm_pmu_hpmn() - Calculate HPMN field value
 * @vcpu: Pointer to struct kvm_vcpu
 *
 * Calculate the appropriate value to set for MDCR_EL2.HPMN. If
 * partitioned, this is the number of counters set for the guest if
 * supported, falling back to hpmn_max if needed. If we are not
 * partitioned or can't set the implied HPMN value, fall back to the
 * host value.
 *
 * Return: A valid HPMN value
 */
u8 kvm_pmu_hpmn(struct kvm_vcpu *vcpu)
{
	u8 nr_guest_cnt = kvm_pmu_guest_num_counters(vcpu);
	int nr_guest_cnt_max = armv8pmu_hpmn_max;
	u8 nr_host_cnt_max = *host_data_ptr(nr_event_counters);

	if (vcpu->kvm->arch.arm_pmu)
		nr_guest_cnt_max = vcpu->kvm->arch.arm_pmu->hpmn_max;

	if (kvm_vcpu_pmu_is_partitioned(vcpu)) {
		if (cpus_have_final_cap(ARM64_HAS_HPMN0))
			return nr_guest_cnt;
		else if (nr_guest_cnt > 0)
			return nr_guest_cnt;
		else if (nr_guest_cnt_max > 0)
			return nr_guest_cnt_max;
	}

	return nr_host_cnt_max;
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
	u64 evtyper_set = ARMV8_PMU_EXCLUDE_EL0 |
		ARMV8_PMU_EXCLUDE_EL1;
	u64 evtyper_clr = ARMV8_PMU_INCLUDE_EL2;
	u8 i;
	u64 val;
	u64 evsel;

	if (!pmu)
		return;

	for (i = 0; i < pmu->hpmn_max; i++) {
		val = __vcpu_sys_reg(vcpu, PMEVTYPER0_EL0 + i);
		evsel = val & kvm_pmu_event_mask(vcpu->kvm);

		if (vcpu->kvm->arch.pmu_filter &&
		    !test_bit(evsel, vcpu->kvm->arch.pmu_filter))
			val |= evtyper_set;

		val &= ~evtyper_clr;
		write_pmevtypern(i, val);
	}

	val = __vcpu_sys_reg(vcpu, PMCCFILTR_EL0);

	if (vcpu->kvm->arch.pmu_filter &&
	    !test_bit(ARMV8_PMUV3_PERFCTR_CPU_CYCLES, vcpu->kvm->arch.pmu_filter))
		val |= evtyper_set;

	val &= ~evtyper_clr;
	write_pmccfiltr(val);
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
	u64 mask;
	u8 i;
	u64 val;

	/*
	 * If we aren't using FGT then we are trapping everything
	 * anyway, so no need to bother with the swap.
	 */
	if (!kvm_vcpu_pmu_use_fgt(vcpu))
		return;

	pmu = vcpu->kvm->arch.arm_pmu;
	kvm_pmu_apply_event_filter(vcpu);

	for (i = 0; i < pmu->hpmn_max; i++) {
		val = __vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + i);
		write_pmevcntrn(i, val);
	}

	val = __vcpu_sys_reg(vcpu, PMCCNTR_EL0);
	write_pmccntr(val);

	val = __vcpu_sys_reg(vcpu, PMUSERENR_EL0);
	write_pmuserenr(val);

	val = __vcpu_sys_reg(vcpu, PMSELR_EL0);
	write_pmselr(val);

	/* Save only the stateful writable bits. */
	val = __vcpu_sys_reg(vcpu, PMCR_EL0);
	mask = ARMV8_PMU_PMCR_MASK &
		~(ARMV8_PMU_PMCR_P | ARMV8_PMU_PMCR_C);
	write_pmcr(val & mask);

	/*
	 * When handling these:
	 * 1. Apply only the bits for guest counters (indicated by mask)
	 * 2. Use the different registers for set and clear
	 */
	mask = kvm_pmu_guest_counter_mask(pmu);

	val = __vcpu_sys_reg(vcpu, PMCNTENSET_EL0);
	write_pmcntenset(val & mask);
	write_pmcntenclr(~val & mask);

	val = __vcpu_sys_reg(vcpu, PMINTENSET_EL1);
	write_pmintenset(val & mask);
	write_pmintenclr(~val & mask);
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
	u64 mask;
	u8 i;
	u64 val;

	/*
	 * If we aren't using FGT then we are trapping everything
	 * anyway, so no need to bother with the swap.
	 */
	if (!kvm_vcpu_pmu_use_fgt(vcpu))
		return;

	pmu = vcpu->kvm->arch.arm_pmu;

	for (i = 0; i < pmu->hpmn_max; i++) {
		val = read_pmevcntrn(i);
		__vcpu_assign_sys_reg(vcpu, PMEVCNTR0_EL0 + i, val);
	}

	val = read_pmccntr();
	__vcpu_assign_sys_reg(vcpu, PMCCNTR_EL0, val);

	val = read_pmuserenr();
	__vcpu_assign_sys_reg(vcpu, PMUSERENR_EL0, val);

	val = read_pmselr();
	__vcpu_assign_sys_reg(vcpu, PMSELR_EL0, val);

	val = read_pmcr();
	__vcpu_assign_sys_reg(vcpu, PMCR_EL0, val);

	/* Mask these to only save the guest relevant bits. */
	mask = kvm_pmu_guest_counter_mask(pmu);

	val = read_pmcntenset();
	__vcpu_assign_sys_reg(vcpu, PMCNTENSET_EL0, val & mask);

	val = read_pmintenset();
	__vcpu_assign_sys_reg(vcpu, PMINTENSET_EL1, val & mask);
}
