// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2024, Amazon.com, Inc. or its affiliates.
 *
 * Tests for pvclock API
 * KVM_SET_CLOCK_GUEST/KVM_GET_CLOCK_GUEST
 */
#include <asm/pvclock.h>
#include <asm/pvclock-abi.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

enum {
	STAGE_FIRST_BOOT,
	STAGE_UNCORRECTED,
	STAGE_CORRECTED
};

#define KVMCLOCK_GPA 0xc0000000ull
#define KVMCLOCK_SIZE sizeof(struct pvclock_vcpu_time_info)

static void trigger_pvti_update(vm_paddr_t pvti_pa)
{
	/*
	 * We need a way to trigger KVM to update the fields
	 * in the PV time info. The easiest way to do this is
	 * to temporarily switch to the old KVM system time
	 * method and then switch back to the new one.
	 */
	wrmsr(MSR_KVM_SYSTEM_TIME, pvti_pa | KVM_MSR_ENABLED);
	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, pvti_pa | KVM_MSR_ENABLED);
}

static void guest_code(vm_paddr_t pvti_pa)
{
	struct pvclock_vcpu_time_info *pvti_va =
		(struct pvclock_vcpu_time_info *)pvti_pa;

	struct pvclock_vcpu_time_info pvti_boot;
	struct pvclock_vcpu_time_info pvti_uncorrected;
	struct pvclock_vcpu_time_info pvti_corrected;
	uint64_t cycles_boot;
	uint64_t cycles_uncorrected;
	uint64_t cycles_corrected;
	uint64_t tsc_guest;

	/*
	 * Setup the KVMCLOCK in the guest & store the original
	 * PV time structure that is used.
	 */
	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, pvti_pa | KVM_MSR_ENABLED);
	pvti_boot = *pvti_va;
	GUEST_SYNC(STAGE_FIRST_BOOT);

	/*
	 * Trigger an update of the PVTI, if we calculate
	 * the KVM clock using this structure we'll see
	 * a delta from the TSC.
	 */
	trigger_pvti_update(pvti_pa);
	pvti_uncorrected = *pvti_va;
	GUEST_SYNC(STAGE_UNCORRECTED);

	/*
	 * The test should have triggered the correction by this
	 * point in time. We have a copy of each of the PVTI structs
	 * at each stage now.
	 *
	 * Let's sample the timestamp at a SINGLE point in time and
	 * then calculate what the KVM clock would be using the PVTI
	 * from each stage.
	 *
	 * Then return each of these values to the tester.
	 */
	pvti_corrected = *pvti_va;
	tsc_guest = rdtsc();

	cycles_boot = __pvclock_read_cycles(&pvti_boot, tsc_guest);
	cycles_uncorrected = __pvclock_read_cycles(&pvti_uncorrected, tsc_guest);
	cycles_corrected = __pvclock_read_cycles(&pvti_corrected, tsc_guest);

	GUEST_SYNC_ARGS(STAGE_CORRECTED, cycles_boot, cycles_uncorrected,
			cycles_corrected, 0);
}

static void run_test(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	struct pvclock_vcpu_time_info pvti_before;
	uint64_t before, uncorrected, corrected;
	int64_t delta_uncorrected, delta_corrected;
	struct ucall uc;
	uint64_t ucall_reason;

	/* Loop through each stage of the test. */
	while (true) {

		/* Start/restart the running vCPU code. */
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		/* Retrieve and verify our stage. */
		ucall_reason = get_ucall(vcpu, &uc);
		TEST_ASSERT(ucall_reason == UCALL_SYNC,
			    "Unhandled ucall reason=%lu",
			    ucall_reason);

		/* Run host specific code relating to stage. */
		switch (uc.args[1]) {
		case STAGE_FIRST_BOOT:
			/* Store the KVM clock values before an update. */
			vcpu_ioctl(vcpu, KVM_GET_CLOCK_GUEST, &pvti_before);

			/* Sleep for a set amount of time to increase delta. */
			sleep(5);
			break;

		case STAGE_UNCORRECTED:
			/* Restore the KVM clock values. */
			vcpu_ioctl(vcpu, KVM_SET_CLOCK_GUEST, &pvti_before);
			break;

		case STAGE_CORRECTED:
			/* Query the clock information and verify delta. */
			before = uc.args[2];
			uncorrected = uc.args[3];
			corrected = uc.args[4];

			delta_uncorrected = before - uncorrected;
			delta_corrected = before - corrected;

			pr_info("before=%lu uncorrected=%lu corrected=%lu\n",
				before, uncorrected, corrected);

			pr_info("delta_uncorrected=%ld delta_corrected=%ld\n",
				delta_uncorrected, delta_corrected);

			TEST_ASSERT((delta_corrected <= 1) && (delta_corrected >= -1),
				    "larger than expected delta detected = %ld", delta_corrected);
			return;
		}
	}
}

static void configure_pvclock(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	unsigned int gpages;

	gpages = vm_calc_num_guest_pages(VM_MODE_DEFAULT, KVMCLOCK_SIZE);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    KVMCLOCK_GPA, 1, gpages, 0);
	virt_map(vm, KVMCLOCK_GPA, KVMCLOCK_GPA, gpages);

	vcpu_args_set(vcpu, 1, KVMCLOCK_GPA);
}

static void configure_scaled_tsc(struct kvm_vcpu *vcpu)
{
	uint64_t tsc_khz;

	tsc_khz =  __vcpu_ioctl(vcpu, KVM_GET_TSC_KHZ, NULL);
	pr_info("scaling tsc from %ldKHz to %ldKHz\n", tsc_khz, tsc_khz / 2);
	tsc_khz /= 2;
	vcpu_ioctl(vcpu, KVM_SET_TSC_KHZ, (void *)tsc_khz);
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	bool scale_tsc;

	scale_tsc = argc > 1 && (!strncmp(argv[1], "-s", 3) ||
				 !strncmp(argv[1], "--scale-tsc", 10));

	TEST_REQUIRE(sys_clocksource_is_based_on_tsc());

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	configure_pvclock(vm, vcpu);

	if (scale_tsc)
		configure_scaled_tsc(vcpu);

	run_test(vm, vcpu);

	return 0;
}
