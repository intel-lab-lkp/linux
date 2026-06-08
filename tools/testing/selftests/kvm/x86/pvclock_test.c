// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © Amazon.com, Inc. or its affiliates.
 *
 * Tests for pvclock API
 * KVM_SET_CLOCK_GUEST/KVM_GET_CLOCK_GUEST
 */
#include <getopt.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#include <asm/pvclock-abi.h>

/*
 * Reproduce the pvclock calculation the guest uses to convert TSC to
 * nanoseconds. This must match the kernel's __pvclock_read_cycles().
 */
static inline uint64_t pvclock_scale_delta(uint64_t delta, uint32_t mul,
					   int8_t shift)
{
	if (shift < 0)
		delta >>= -shift;
	else
		delta <<= shift;
	return ((__uint128_t)delta * mul) >> 32;
}

static inline uint64_t pvclock_read_cycles(struct pvclock_vcpu_time_info *src,
					   uint64_t tsc)
{
	uint64_t delta = tsc - src->tsc_timestamp;

	return src->system_time + pvclock_scale_delta(delta,
						      src->tsc_to_system_mul,
						      src->tsc_shift);
}

static inline void pvti_snapshot(struct pvclock_vcpu_time_info *dst,
				 volatile struct pvclock_vcpu_time_info *src)
{
	uint32_t version;

	do {
		version = src->version;
		__asm__ __volatile__("" ::: "memory");
		*dst = *src;
		__asm__ __volatile__("" ::: "memory");
	} while ((src->version & 1) || src->version != version);
}

enum {
	STAGE_FIRST_BOOT,
	STAGE_UNCORRECTED,
	STAGE_CORRECTED
};

#define KVMCLOCK_GPA	0xc0000000ull
#define KVMCLOCK_SIZE	sizeof(struct pvclock_vcpu_time_info)

static void trigger_pvti_update(void)
{
	/*
	 * Toggle between KVM's old and new system time methods to coerce KVM
	 * into updating the fields in the PV time info struct.
	 */
	wrmsr(MSR_KVM_SYSTEM_TIME, KVMCLOCK_GPA | KVM_MSR_ENABLED);
	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, KVMCLOCK_GPA | KVM_MSR_ENABLED);
}

static void guest_code(void)
{
	struct pvclock_vcpu_time_info *pvti =
		(void *)(unsigned long)KVMCLOCK_GPA;
	struct pvclock_vcpu_time_info pvti_boot;
	struct pvclock_vcpu_time_info pvti_uncorrected;
	struct pvclock_vcpu_time_info pvti_corrected;
	uint64_t tsc_guest;
	uint64_t clk_boot, clk_uncorrected, clk_corrected;
	int64_t delta_corrected;

	/* Set up kvmclock and snapshot the initial pvclock parameters. */
	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, KVMCLOCK_GPA | KVM_MSR_ENABLED);
	pvti_snapshot(&pvti_boot, pvti);
	GUEST_SYNC(STAGE_FIRST_BOOT);

	/*
	 * Trigger an update of the PVTI. Calculating the KVM clock using this
	 * updated structure will show a delta from the original.
	 */
	trigger_pvti_update();
	pvti_snapshot(&pvti_uncorrected, pvti);
	GUEST_SYNC(STAGE_UNCORRECTED);

	/*
	 * Snapshot the corrected time (the host does KVM_SET_CLOCK_GUEST when
	 * handling STAGE_UNCORRECTED).
	 */
	pvti_snapshot(&pvti_corrected, pvti);

	/*
	 * Sample the TSC at a single point in time, then calculate the
	 * effective KVM clock using the PVTI from each stage. Verify that the
	 * corrected clock matches the boot clock to within ±2ns.
	 */
	tsc_guest = rdtsc();

	clk_boot = pvclock_read_cycles(&pvti_boot, tsc_guest);
	clk_uncorrected = pvclock_read_cycles(&pvti_uncorrected, tsc_guest);
	clk_corrected = pvclock_read_cycles(&pvti_corrected, tsc_guest);

	delta_corrected = clk_boot - clk_corrected;

	__GUEST_ASSERT(delta_corrected >= -2 && delta_corrected <= 2,
		       "corrected delta %ld out of range (boot=%lu uncorrected=%lu corrected=%lu)",
		       delta_corrected, clk_boot, clk_uncorrected, clk_corrected);

	GUEST_SYNC(STAGE_CORRECTED);
}

static void run_test(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
		     unsigned int sleep_sec)
{
	struct pvclock_vcpu_time_info pvti_before;
	struct ucall uc;

	for (;;) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_SYNC:
			break;
		default:
			TEST_FAIL("Unexpected ucall");
		}

		switch (uc.args[1]) {
		case STAGE_FIRST_BOOT:
			/* Save the pvclock parameters before the update. */
			vcpu_ioctl(vcpu, KVM_GET_CLOCK_GUEST, &pvti_before);

			/* Sleep to let the clocks diverge. */
			sleep(sleep_sec);
			break;

		case STAGE_UNCORRECTED:
			/* Restore the original pvclock parameters. */
			vcpu_ioctl(vcpu, KVM_SET_CLOCK_GUEST, &pvti_before);
			break;

		case STAGE_CORRECTED:
			/* Guest verified the delta in-guest. */
			return;

		default:
			TEST_FAIL("Unknown stage %lu", uc.args[1]);
		}
	}
}

static void configure_pvclock(struct kvm_vm *vm)
{
	unsigned int nr_pages;

	nr_pages = vm_calc_num_guest_pages(VM_MODE_DEFAULT, getpagesize());
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    KVMCLOCK_GPA, 1, nr_pages, 0);
	virt_map(vm, KVMCLOCK_GPA, KVMCLOCK_GPA, nr_pages);
}

static void run_at_frequency(uint64_t tsc_khz, unsigned int sleep_sec)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	pr_info("Testing at TSC frequency %lu kHz\n", tsc_khz);
	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	configure_pvclock(vm);
	vcpu_ioctl(vcpu, KVM_SET_TSC_KHZ, (void *)tsc_khz);
	run_test(vm, vcpu, sleep_sec);
	kvm_vm_free(vm);
}

static void test_tsc_stable_bit(void);
static void test_clock_guest_with_offsets(void);

static void usage(const char *name)
{
	printf("Usage: %s [options]\n"
	       "  -s, --sleep SEC     sleep duration between snapshots (default: 2)\n"
	       "  -h, --help          show this help\n", name);
}

int main(int argc, char *argv[])
{
	static const struct option long_opts[] = {
		{ "sleep", required_argument, NULL, 's' },
		{ "help",  no_argument,       NULL, 'h' },
		{ NULL,    0,                  NULL,  0  },
	};
	unsigned int sleep_sec = 2;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	uint64_t host_khz;
	uint64_t freq;
	int opt;

	while ((opt = getopt_long(argc, argv, "s:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			sleep_sec = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	TEST_REQUIRE(sys_clocksource_is_based_on_tsc());
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_TSC_CONTROL));

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	configure_pvclock(vm);

	/* Check KVM_GET_CLOCK_GUEST is supported */
	{
		struct pvclock_vcpu_time_info tmp;
		int ret = __vcpu_ioctl(vcpu, KVM_GET_CLOCK_GUEST, &tmp);
		TEST_REQUIRE(ret != -1 || errno != ENOTTY);
	}

	/* First run at native frequency (no scaling). */
	run_test(vm, vcpu, sleep_sec);

	/*
	 * Then enumerate a range of TSC frequencies crossing the 32-bit
	 * boundary, to exercise the scaling path at various ratios.
	 */
	host_khz = __vcpu_ioctl(vcpu, KVM_GET_TSC_KHZ, NULL);
	kvm_vm_free(vm);

	for (freq = 1000000; freq <= 5000000; freq += 500000) {
		if (freq == host_khz)
			continue;
		run_at_frequency(freq, sleep_sec);
	}

	test_tsc_stable_bit();
	test_clock_guest_with_offsets();

	return 0;
}

static volatile uint32_t vcpu_counter;

static void guest_code_stable_bit(void)
{
	uint32_t idx = __atomic_fetch_add(&vcpu_counter, 1, __ATOMIC_SEQ_CST);
	uint64_t gpa = KVMCLOCK_GPA + idx * sizeof(struct pvclock_vcpu_time_info);

	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, gpa | KVM_MSR_ENABLED);
	GUEST_SYNC(0);
	GUEST_SYNC(0);
	GUEST_SYNC(0);
}

static void set_tsc_offset(struct kvm_vcpu *vcpu, uint64_t offset)
{
	struct kvm_device_attr attr = {
		.group = KVM_VCPU_TSC_CTRL,
		.attr = KVM_VCPU_TSC_OFFSET,
		.addr = (__u64)(uintptr_t)&offset,
	};
	vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &attr);
}

static void run_vcpu_once(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_SYNC:
		break;
	default:
		TEST_FAIL("Unexpected ucall");
	}
}

static void test_tsc_stable_bit(void)
{
	struct pvclock_vcpu_time_info pvti;
	struct kvm_vcpu *vcpus[2];
	struct kvm_vm *vm;
	int ret;

	pr_info("Testing PVCLOCK_TSC_STABLE_BIT with matched/unmatched TSCs\n");

	vm = vm_create_with_vcpus(2, guest_code_stable_bit, vcpus);
	configure_pvclock(vm);

	/*
	 * Case 1: All TSCs matched (same frequency and offset).
	 * Master clock should be active, PVCLOCK_TSC_STABLE_BIT set.
	 */
	run_vcpu_once(vcpus[0]);

	ret = __vcpu_ioctl(vcpus[0], KVM_GET_CLOCK_GUEST, &pvti);
	TEST_ASSERT(!ret, "GET_CLOCK_GUEST should succeed with matched TSCs");
	TEST_ASSERT(pvti.flags & PVCLOCK_TSC_STABLE_BIT,
		    "PVCLOCK_TSC_STABLE_BIT should be set with matched TSCs");

	/*
	 * Case 2: Different TSC offset, same frequency.
	 * Master clock should still be active (frequency matches), but
	 * PVCLOCK_TSC_STABLE_BIT should be cleared (offsets differ).
	 */
	set_tsc_offset(vcpus[1], 12345678);
	run_vcpu_once(vcpus[1]);
	run_vcpu_once(vcpus[0]);

	ret = __vcpu_ioctl(vcpus[0], KVM_GET_CLOCK_GUEST, &pvti);
	if (ret) {
		/* Master clock disabled by offset mismatch — old kernel */
		pr_info("  Skipping offset tests (master clock requires matched offsets)\n");
		goto out_stable;
	}
	TEST_ASSERT(!(pvti.flags & PVCLOCK_TSC_STABLE_BIT),
		    "PVCLOCK_TSC_STABLE_BIT should be clear with offset-mismatched TSCs");

	/*
	 * Case 3: Different TSC frequency.
	 * Master clock should be disabled entirely.
	 */
	vcpu_ioctl(vcpus[1], KVM_SET_TSC_KHZ,
		   (void *)(unsigned long)(__vcpu_ioctl(vcpus[1], KVM_GET_TSC_KHZ, NULL) / 2));
	/* Write TSC to trigger kvm_synchronize_tsc / kvm_track_tsc_matching */
	vcpu_set_msr(vcpus[1], MSR_IA32_TSC, 0);
	run_vcpu_once(vcpus[1]);

	ret = __vcpu_ioctl(vcpus[0], KVM_GET_CLOCK_GUEST, &pvti);
	TEST_ASSERT(ret && errno == EINVAL,
		    "GET_CLOCK_GUEST should fail with frequency-mismatched TSCs, got %d (errno %d)",
		    ret, errno);

out_stable:
	kvm_vm_free(vm);
}

static void test_clock_guest_with_offsets(void)
{
	struct pvclock_vcpu_time_info pvti0, pvti1, pvti1_after;
	struct kvm_vcpu *vcpus[2];
	struct kvm_vm *vm;
	int64_t delta;
	int ret;

	pr_info("Testing KVM_[GS]ET_CLOCK_GUEST with different TSC offsets\n");

	vm = vm_create_with_vcpus(2, guest_code_stable_bit, vcpus);
	configure_pvclock(vm);

	/* Set different TSC offsets on the two vCPUs */
	set_tsc_offset(vcpus[0], 0);
	set_tsc_offset(vcpus[1], 1000000000ull);

	/* Run both to establish kvmclock */
	run_vcpu_once(vcpus[0]);
	run_vcpu_once(vcpus[1]);

	/* GET_CLOCK_GUEST on both — should succeed (master clock active) */
	ret = __vcpu_ioctl(vcpus[0], KVM_GET_CLOCK_GUEST, &pvti0);
	if (ret) {
		pr_info("  Skipping (master clock requires matched offsets on this kernel)\n");
		kvm_vm_free(vm);
		return;
	}
	ret = __vcpu_ioctl(vcpus[1], KVM_GET_CLOCK_GUEST, &pvti1);
	TEST_ASSERT(!ret, "GET_CLOCK_GUEST on vcpu1 failed");

	/* The tsc_timestamps should differ (different offsets) */
	TEST_ASSERT(pvti0.tsc_timestamp != pvti1.tsc_timestamp,
		    "tsc_timestamps should differ with different offsets");

	/* Sleep to let time elapse, then restore vcpu0's clock */
	sleep(1);
	vcpu_ioctl(vcpus[0], KVM_SET_CLOCK_GUEST, &pvti0);

	/* Run vcpu0 to process the clock update */
	run_vcpu_once(vcpus[0]);

	/* GET_CLOCK_GUEST on vcpu1 — should reflect the correction */
	ret = __vcpu_ioctl(vcpus[1], KVM_GET_CLOCK_GUEST, &pvti1_after);
	TEST_ASSERT(!ret, "GET_CLOCK_GUEST on vcpu1 after SET failed");

	/*
	 * After SET on vcpu0, verify the correction worked by getting
	 * the clock on vcpu0 again. The mul/shift should be the same,
	 * and computing kvmclock at the same TSC should give the same
	 * result as the original (within ±2ns).
	 */
	{
		struct pvclock_vcpu_time_info pvti0_after;
		uint64_t tsc_now, clk_from_old, clk_from_new;

		ret = __vcpu_ioctl(vcpus[0], KVM_GET_CLOCK_GUEST, &pvti0_after);
		TEST_ASSERT(!ret, "GET_CLOCK_GUEST on vcpu0 after SET failed");

		tsc_now = pvti0_after.tsc_timestamp;
		clk_from_old = pvclock_read_cycles(&pvti0, tsc_now);
		clk_from_new = pvclock_read_cycles(&pvti0_after, tsc_now);

		delta = (int64_t)clk_from_new - (int64_t)clk_from_old;
		TEST_ASSERT(delta >= -2 && delta <= 2,
			    "clock correction delta should be <=2ns, got %ld ns",
			    delta);
	}

	/*
	 * Also verify that vcpu1's clock is still accessible (master
	 * clock still active with different offsets).
	 */
	ret = __vcpu_ioctl(vcpus[1], KVM_GET_CLOCK_GUEST, &pvti1_after);
	TEST_ASSERT(!ret, "GET_CLOCK_GUEST on vcpu1 after SET failed");

	kvm_vm_free(vm);
}
