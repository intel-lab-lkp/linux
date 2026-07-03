// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test that KVM master clock mode works with different TSC offsets
 * as long as all vCPUs have the same TSC frequency.
 */
#include <stdint.h>
#include <string.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#include <asm/pvclock-abi.h>

#define KVMCLOCK_GPA	0xc0000000ull
#define TSC_OFFSET	(1000000000ULL)

static uint64_t pvclock_calc(struct pvclock_vcpu_time_info *pvti, uint64_t guest_tsc)
{
	uint64_t delta = guest_tsc - pvti->tsc_timestamp;

	if (pvti->tsc_shift >= 0)
		delta <<= pvti->tsc_shift;
	else
		delta >>= -(int)pvti->tsc_shift;

	return pvti->system_time + ((__uint128_t)delta * pvti->tsc_to_system_mul >> 32);
}

static void guest_code(void)
{
	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, KVMCLOCK_GPA | KVM_MSR_ENABLED);
	for (;;)
		GUEST_SYNC(0);
}

int main(void)
{
	struct kvm_vcpu *vcpus[3];
	struct kvm_clock_data clock;
	struct pvclock_vcpu_time_info pvti[3];
	struct kvm_vm *vm;
	uint64_t offset0, host_tsc, clk0, clk2;
	int i;

	TEST_REQUIRE(sys_clocksource_is_based_on_tsc());

	vm = vm_create_with_vcpus(3, guest_code, vcpus);

	TEST_REQUIRE(!__vcpu_has_device_attr(vcpus[0], KVM_VCPU_TSC_CTRL,
					     KVM_VCPU_TSC_OFFSET));

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    KVMCLOCK_GPA, 1,
				    vm_calc_num_guest_pages(VM_MODE_DEFAULT,
							   getpagesize()), 0);
	virt_map(vm, KVMCLOCK_GPA, KVMCLOCK_GPA,
		 vm_calc_num_guest_pages(VM_MODE_DEFAULT, getpagesize()));

	/* Get vCPU 0's default offset and set vCPU 2's offset higher */
	vcpu_device_attr_get(vcpus[0], KVM_VCPU_TSC_CTRL,
			     KVM_VCPU_TSC_OFFSET, &offset0);
	uint64_t offset2 = offset0 + TSC_OFFSET;
	vcpu_device_attr_set(vcpus[2], KVM_VCPU_TSC_CTRL,
			     KVM_VCPU_TSC_OFFSET, &offset2);

	/* Run each vCPU to enable kvmclock (with offset already set) */
	for (i = 0; i < 3; i++) {
		vcpu_run(vcpus[i]);
		TEST_ASSERT_KVM_EXIT_REASON(vcpus[i], KVM_EXIT_IO);
	}

	/* Check master clock is active */
	memset(&clock, 0, sizeof(clock));
	vm_ioctl(vm, KVM_GET_CLOCK, &clock);
	pr_info("KVM_GET_CLOCK flags: 0x%x\n", clock.flags);
	TEST_ASSERT(clock.flags & KVM_CLOCK_HOST_TSC,
		    "Master clock should be active, flags=0x%x", clock.flags);
	TEST_ASSERT(clock.flags & KVM_CLOCK_TSC_STABLE,
		    "KVM_CLOCK_TSC_STABLE should be set, flags=0x%x", clock.flags);

	/* Get per-vCPU pvclock in order 0, 2, 1 */
	int order[] = {0, 2, 1};
	for (i = 0; i < 3; i++) {
		int idx = order[i];
		__vcpu_ioctl(vcpus[idx], KVM_GET_CLOCK_GUEST, &pvti[idx]);
		pr_info("vCPU %d: tsc_timestamp=%lu system_time=%lu "
			"mul=%u shift=%d flags=0x%x\n",
			idx, (unsigned long)pvti[idx].tsc_timestamp,
			(unsigned long)pvti[idx].system_time,
			pvti[idx].tsc_to_system_mul, pvti[idx].tsc_shift,
			pvti[idx].flags);
	}

	/* Read guest TSCs: should see (0+OFF) < 2 < (1+OFF) */
	uint64_t gtsc0 = vcpu_get_msr(vcpus[0], MSR_IA32_TSC);
	uint64_t gtsc2 = vcpu_get_msr(vcpus[2], MSR_IA32_TSC);
	uint64_t gtsc1 = vcpu_get_msr(vcpus[1], MSR_IA32_TSC);
	pr_info("Guest TSCs: vcpu0=%lu vcpu2=%lu vcpu1=%lu\n",
		(unsigned long)gtsc0, (unsigned long)gtsc2, (unsigned long)gtsc1);
	pr_info("vcpu0+OFF=%lu vcpu1+OFF=%lu\n",
		(unsigned long)(gtsc0 + TSC_OFFSET),
		(unsigned long)(gtsc1 + TSC_OFFSET));
	TEST_ASSERT(gtsc0 + TSC_OFFSET < gtsc2 && gtsc2 < gtsc1 + TSC_OFFSET,
		    "Expected (vcpu0+OFF) < vcpu2 < (vcpu1+OFF)");

	/* PVCLOCK_TSC_STABLE_BIT should NOT be set (offsets differ) */
	TEST_ASSERT(!(pvti[2].flags & PVCLOCK_TSC_STABLE_BIT),
		    "PVCLOCK_TSC_STABLE_BIT should NOT be set, flags=0x%x",
		    pvti[2].flags);

	/* Same mul/shift */
	TEST_ASSERT(pvti[0].tsc_to_system_mul == pvti[2].tsc_to_system_mul &&
		    pvti[0].tsc_shift == pvti[2].tsc_shift,
		    "All vCPUs should have same mul/shift");

	/*
	 * Read host TSC once. At this instant:
	 *   vCPU 0 guest TSC = host_tsc + offset0
	 *   vCPU 2 guest TSC = host_tsc + offset0 + TSC_OFFSET
	 * Feed each through its pvclock. Expect the same kvmclock.
	 */
	host_tsc = rdtsc();
	clk0 = pvclock_calc(&pvti[0], host_tsc + offset0);
	clk2 = pvclock_calc(&pvti[2], host_tsc + offset0 + TSC_OFFSET);

	pr_info("kvmclock via vCPU 0: %lu ns\n", (unsigned long)clk0);
	pr_info("kvmclock via vCPU 2: %lu ns\n", (unsigned long)clk2);
	TEST_ASSERT(clk0 == clk2,
		    "kvmclock from offset vCPUs should match exactly, "
		    "diff=%ld ns", (long)(clk2 - clk0));

	pr_info("PASSED: pvclock consistent across offset vCPUs\n");

	/*
	 * Now add an hour to the VM kvmclock via KVM_SET_CLOCK, run each
	 * vCPU to pick up the update, and check they're still in sync.
	 */
	{
#define ONE_HOUR_NS (3600ULL * NSEC_PER_SEC)
		struct kvm_clock_data setclk = { .clock = clock.clock + ONE_HOUR_NS };

		vm_ioctl(vm, KVM_SET_CLOCK, &setclk);
	}

	/* Guest code does GUEST_SYNC then exits — run each to see update */
	for (i = 0; i < 3; i++) {
		vcpu_run(vcpus[order[i]]);
		TEST_ASSERT_KVM_EXIT_REASON(vcpus[order[i]], KVM_EXIT_IO);
	}

	/* Re-read pvclocks */
	for (i = 0; i < 3; i++)
		__vcpu_ioctl(vcpus[order[i]], KVM_GET_CLOCK_GUEST, &pvti[order[i]]);

	pr_info("After +1h: vCPU 0 system_time=%lu, vCPU 2 system_time=%lu\n",
		(unsigned long)pvti[0].system_time,
		(unsigned long)pvti[2].system_time);
	TEST_ASSERT(pvti[0].system_time == pvti[2].system_time,
		    "system_time should still match after KVM_SET_CLOCK");

	host_tsc = rdtsc();
	clk0 = pvclock_calc(&pvti[0], host_tsc + offset0);
	clk2 = pvclock_calc(&pvti[2], host_tsc + offset0 + TSC_OFFSET);

	pr_info("After +1h: kvmclock via vCPU 0: %lu ns\n", (unsigned long)clk0);
	pr_info("After +1h: kvmclock via vCPU 2: %lu ns\n", (unsigned long)clk2);
	TEST_ASSERT(clk0 == clk2,
		    "After +1h: kvmclock should still match, diff=%ld ns",
		    (long)(clk2 - clk0));

	/* Verify the clock actually moved by ~1 hour */
	TEST_ASSERT(clk0 > ONE_HOUR_NS,
		    "Clock should be > 1 hour after set, got %lu ns",
		    (unsigned long)clk0);

	pr_info("PASSED: pvclock still consistent after KVM_SET_CLOCK +1h\n");
	kvm_vm_free(vm);
	return 0;
}
