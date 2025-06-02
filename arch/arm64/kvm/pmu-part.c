// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/perf/arm_pmu.h>
#include <linux/perf/arm_pmuv3.h>

#include <asm/kvm_emulate.h>
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
	u64 mask = kvm_pmu_guest_counter_mask(pmu);
	u8 i;
	u64 val;

	/*
	 * If the PMU is not partitioned, don't bother.
	 *
	 * If we have MDCR_EL2_TPM, every PMU access is trapped which
	 * implies we are using the emulated PMU instead of direct
	 * access.
	 */
	if (!kvm_pmu_is_partitioned(pmu) || (vcpu->arch.mdcr_el2 & MDCR_EL2_TPM))
		return;

	for (i = 0; i < pmu->hpmn; i++) {
		val = __vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + i);
		write_pmevcntrn(i, val);
	}

	val = __vcpu_sys_reg(vcpu, PMCCNTR_EL0);
	write_pmccntr(val);

	if (cpus_have_final_cap(ARM64_HAS_PMICNTR)) {
		val = __vcpu_sys_reg(vcpu, PMICNTR_EL0);
		write_pmicntr(val);
	}

	val = __vcpu_sys_reg(vcpu, PMUSERENR_EL0);
	write_pmuserenr(val);

	val = __vcpu_sys_reg(vcpu, PMSELR_EL0);
	write_pmselr(val);

	val = __vcpu_sys_reg(vcpu, PMCR_EL0);
	write_pmcr(val);

	/*
	 * Loading these registers is more intricate because of
	 * 1. Applying only the bits for guest counters (indicated by mask)
	 * 2. Setting and clearing are different registers
	 */
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
	struct arm_pmu *pmu = vcpu->kvm->arch.arm_pmu;
	u64 mask = kvm_pmu_guest_counter_mask(pmu);
	u8 i;
	u64 val;

	/*
	 * If the PMU is not partitioned, don't bother.
	 *
	 * If we have MDCR_EL2_TPM, every PMU access is trapped which
	 * implies we are using the emulated PMU instead of direct
	 * access.
	 */
	if (!kvm_pmu_is_partitioned(pmu) || (vcpu->arch.mdcr_el2 & MDCR_EL2_TPM))
		return;

	for (i = 0; i < pmu->hpmn; i++) {
		val = read_pmevcntrn(i);
		__vcpu_sys_reg(vcpu, PMEVCNTR0_EL0 + i) = val;
	}

	val = read_pmccntr();
	__vcpu_sys_reg(vcpu, PMCCNTR_EL0) = val;

	if (this_cpu_has_cap(ARM64_HAS_PMICNTR)) {
		val = read_pmicntr();
		__vcpu_sys_reg(vcpu, PMICNTR_EL0) = val;
	}

	val = read_pmuserenr();
	__vcpu_sys_reg(vcpu, PMUSERENR_EL0) = val;

	val = read_pmselr();
	__vcpu_sys_reg(vcpu, PMSELR_EL0) = val;

	val = read_pmcr();
	__vcpu_sys_reg(vcpu, PMCR_EL0) = val;

	/* Mask these to only save the guest relevant bits. */
	val = read_pmcntenset();
	__vcpu_sys_reg(vcpu, PMCNTENSET_EL0) = val & mask;

	val = read_pmintenset();
	__vcpu_sys_reg(vcpu, PMINTENSET_EL1) = val & mask;
}

/**
 * kvm_pmu_handle_guest_irq() - Record IRQs in guest counters
 * @govf: Bitmask of guest overflowed counters
 *
 * Record IRQs from overflows in guest-reserved counters in the VCPU
 * register for the guest to clear later.
 */
void kvm_pmu_handle_guest_irq(u64 govf)
{
	struct kvm_vcpu *vcpu = kvm_get_running_vcpu();

	if (!vcpu)
		return;

	__vcpu_sys_reg(vcpu, PMOVSSET_EL0) |= govf;
}

/**
 * kvm_pmu_part_overflow_status() - Determine if any guest counters have overflowed
 * @vcpu: Ponter to struct kvm_vcpu
 *
 * Determine if any guest counters have overflowed and therefore an
 * IRQ needs to be injected into the guest.
 *
 * Return: True if there was an overflow, false otherwise
 */
bool kvm_pmu_part_overflow_status(struct kvm_vcpu *vcpu)
{
	struct arm_pmu *pmu = vcpu->kvm->arch.arm_pmu;
	u64 mask = kvm_pmu_guest_counter_mask(pmu);
	u64 pmovs = __vcpu_sys_reg(vcpu, PMOVSSET_EL0);
	u64 pmint = read_pmintenset();
	u64 pmcr = read_pmcr();

	return (pmcr & ARMV8_PMU_PMCR_E) && (mask & pmovs & pmint);
}
