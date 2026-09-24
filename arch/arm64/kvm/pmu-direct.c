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
