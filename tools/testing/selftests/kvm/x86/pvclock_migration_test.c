// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test KVM clock precision across simulated live migration.
 *
 * Verifies that the documented TSC migration procedure (using
 * KVM_VCPU_TSC_OFFSET, KVM_VCPU_TSC_SCALE, KVM_GET_CLOCK, and
 * KVM_SET_CLOCK_GUEST) preserves the kvmclock's relationship to
 * CLOCK_MONOTONIC_RAW.
 *
 * The test:
 * 1. Creates a VM, runs the guest to enable kvmclock
 * 2. Does a PTP-like ABA measurement of kvmclock vs CLOCK_MONOTONIC_RAW
 * 3. Follows the documented migration procedure (same host, 1s pause)
 * 4. Does the same ABA measurement on the destination VM
 * 5. Verifies the kvmclock-vs-monotonic delta is preserved
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#include <asm/pvclock-abi.h>

#define KVMCLOCK_GPA	0xc0000000ULL

static void guest_code(void)
{
	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, KVMCLOCK_GPA | 1);
	GUEST_SYNC(0);
	GUEST_SYNC(1);
}

static uint64_t read_kvmclock_ns(struct kvm_vm *vm)
{
	struct kvm_clock_data data = {};

	vm_ioctl(vm, KVM_GET_CLOCK, &data);
	return data.clock;
}

static uint64_t pvclock_read_cycles(struct pvclock_vcpu_time_info *src,
				    uint64_t tsc)
{
	uint64_t delta = tsc - src->tsc_timestamp;
	uint64_t ns;

	if (src->tsc_shift >= 0)
		delta <<= src->tsc_shift;
	else
		delta >>= -(int32_t)src->tsc_shift;

	ns = (unsigned __int128)delta * src->tsc_to_system_mul >> 32;
	return src->system_time + ns;
}

/*
 * ABA measurement: read CLOCK_MONOTONIC_RAW, kvmclock, CLOCK_MONOTONIC_RAW.
 * Repeat 3 times, keep the reading with the smallest spread.
 */
static void aba_reading(struct kvm_vm *vm, uint64_t *lo, uint64_t *kvm_ns,
			uint64_t *hi)
{
	uint64_t best_spread = UINT64_MAX;
	int i;

	for (i = 0; i < 3; i++) {
		struct timespec ts1, ts2;
		uint64_t m1, m2, clk;

		clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
		clk = read_kvmclock_ns(vm);
		clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);

		m1 = ts1.tv_sec * 1000000000ULL + ts1.tv_nsec;
		m2 = ts2.tv_sec * 1000000000ULL + ts2.tv_nsec;

		if (m2 - m1 < best_spread) {
			best_spread = m2 - m1;
			*lo = m1;
			*kvm_ns = clk;
			*hi = m2;
		}
	}
}

static struct kvm_vm *create_vm(struct kvm_vcpu **vcpu)
{
	struct kvm_vm *vm = vm_create_with_one_vcpu(vcpu, guest_code);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    KVMCLOCK_GPA, 1, 1, 0);
	virt_map(vm, KVMCLOCK_GPA, KVMCLOCK_GPA, 1);
	return vm;
}

int main(void)
{
	struct pvclock_vcpu_time_info pvti_src;
	struct kvm_clock_data clock_src, clock_dst;
	struct kvm_vcpu_tsc_scale scale_src, scale_dst;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	uint64_t mono_before, kvm_before, kvm_after;
	int64_t delta_before;
	uint64_t ofs_src, tsc_src, tsc_dst, raw_dst, ofs_dst;
	uint64_t host_tsc_src, host_tsc_dst;
	uint64_t time_src, time_dst;
	int64_t delta_t;
	uint32_t freq_khz = 1500000; /* 1.5 GHz — forces TSC scaling */
	int ret;

	TEST_REQUIRE(sys_clocksource_is_based_on_tsc());

	/* === SOURCE SIDE === */
	pr_info("=== Source VM ===\n");
	vm = create_vm(&vcpu);

	/* Set guest TSC frequency (may trigger scaling) */
	vcpu_ioctl(vcpu, KVM_SET_TSC_KHZ, (void *)(unsigned long)freq_khz);

	/* Run guest to enable kvmclock */
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);

	/* ABA measurement: kvmclock vs CLOCK_MONOTONIC_RAW */
	uint64_t src_mono_lo, src_mono_hi;
	aba_reading(vm, &src_mono_lo, &kvm_before, &src_mono_hi);
	mono_before = (src_mono_lo + src_mono_hi) / 2;
	delta_before = (int64_t)(kvm_before - mono_before);
	pr_info("  kvmclock - MONOTONIC_RAW = %" PRId64 " ns (±%" PRIu64 " ns)\n",
		delta_before, (src_mono_hi - src_mono_lo) / 2);

	/* Step 1: KVM_GET_CLOCK for atomic {host_tsc, realtime} */
	memset(&clock_src, 0, sizeof(clock_src));
	clock_src.flags = KVM_CLOCK_REALTIME;
	vm_ioctl(vm, KVM_GET_CLOCK, &clock_src);
	host_tsc_src = clock_src.host_tsc;
	time_src = clock_src.realtime;

	/* Step 2: Save TSC offset and scale */
	{
		struct kvm_device_attr attr = {
			.group = KVM_VCPU_TSC_CTRL,
			.attr = KVM_VCPU_TSC_OFFSET,
			.addr = (uint64_t)(uintptr_t)&ofs_src,
		};
		vcpu_ioctl(vcpu, KVM_GET_DEVICE_ATTR, &attr);
	}
	{
		struct kvm_device_attr attr = {
			.group = KVM_VCPU_TSC_CTRL,
			.attr = KVM_VCPU_TSC_SCALE,
			.addr = (uint64_t)(uintptr_t)&scale_src,
		};
		memset(&scale_src, 0, sizeof(scale_src));
		__vcpu_ioctl(vcpu, KVM_GET_DEVICE_ATTR, &attr);
	}

	/* Compute guest TSC at Tsrc */
	if (scale_src.tsc_frac_bits)
		tsc_src = ((unsigned __int128)host_tsc_src * scale_src.tsc_ratio
			   >> scale_src.tsc_frac_bits) + ofs_src;
	else
		tsc_src = host_tsc_src + ofs_src;

	/* Step 3: KVM_GET_CLOCK_GUEST */
	ret = __vcpu_ioctl(vcpu, KVM_GET_CLOCK_GUEST, &pvti_src);
	TEST_ASSERT(!ret, "KVM_GET_CLOCK_GUEST failed");

	pr_info("  TSC freq=%u kHz, offset=%" PRId64 "\n", freq_khz, (int64_t)ofs_src);

	kvm_vm_release(vm);

	/* === PAUSE (simulate migration) === */
	pr_info("=== Pausing 1 second ===\n");
	sleep(1);

	/* === DESTINATION SIDE === */
	pr_info("=== Destination VM ===\n");
	vm = create_vm(&vcpu);

	/* Step 4: KVM_SET_TSC_KHZ */
	vcpu_ioctl(vcpu, KVM_SET_TSC_KHZ, (void *)(unsigned long)freq_khz);

	/* Step 5: KVM_GET_CLOCK for atomic {host_tsc, realtime} pair.
	 * Master clock is active from vCPU creation.
	 */
	memset(&clock_dst, 0, sizeof(clock_dst));
	vm_ioctl(vm, KVM_GET_CLOCK, &clock_dst);
	host_tsc_dst = clock_dst.host_tsc;
	time_dst = clock_dst.realtime;

	/* Step 6: ΔT */
	delta_t = (int64_t)(time_dst - time_src);

	/* Step 7: Compute destination offset */
	{
		struct kvm_device_attr attr = {
			.group = KVM_VCPU_TSC_CTRL,
			.attr = KVM_VCPU_TSC_SCALE,
			.addr = (uint64_t)(uintptr_t)&scale_dst,
		};
		memset(&scale_dst, 0, sizeof(scale_dst));
		__vcpu_ioctl(vcpu, KVM_GET_DEVICE_ATTR, &attr);
	}

	tsc_dst = tsc_src + (uint64_t)((int64_t)freq_khz * 1000 * delta_t / 1000000000LL);

	if (scale_dst.tsc_frac_bits)
		raw_dst = (unsigned __int128)host_tsc_dst * scale_dst.tsc_ratio
			  >> scale_dst.tsc_frac_bits;
	else
		raw_dst = host_tsc_dst;

	ofs_dst = tsc_dst - raw_dst;

	/*
	 * The TSC offset delta introduced by using CLOCK_REALTIME to
	 * estimate elapsed time. On same host, the correct offset is
	 * ofs_src; the difference is the CLOCK_REALTIME-vs-TSC error.
	 */
	int64_t tsc_ofs_delta = (int64_t)(ofs_dst - ofs_src);
	int64_t tsc_ofs_delta_ns = tsc_ofs_delta * 1000000000LL / ((int64_t)freq_khz * 1000);
	pr_info("  Destination TSC offset=%" PRId64
		", imprecision from CLOCK_REALTIME: %" PRId64 " cycles = %"
		PRId64 " ns\n", (int64_t)ofs_dst, tsc_ofs_delta, tsc_ofs_delta_ns);

	/* Set TSC offset */
	{
		struct kvm_device_attr attr = {
			.group = KVM_VCPU_TSC_CTRL,
			.attr = KVM_VCPU_TSC_OFFSET,
			.addr = (uint64_t)(uintptr_t)&ofs_dst,
		};
		vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &attr);
	}

	/* Step 8: KVM_SET_CLOCK_GUEST */
	ret = __vcpu_ioctl(vcpu, KVM_SET_CLOCK_GUEST, &pvti_src);
	TEST_ASSERT(!ret, "KVM_SET_CLOCK_GUEST failed: errno %d", errno);

	/* Run guest to update pvclock page on destination */
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);

	/* ABA measurement on destination */
	uint64_t mono_lo, mono_hi;
	aba_reading(vm, &mono_lo, &kvm_after, &mono_hi);

	/*
	 * The kvmclock is tied to the guest TSC via SET_CLOCK_GUEST.
	 * The guest TSC is offset from the correct value by tsc_ofs_delta_ns
	 * (due to CLOCK_REALTIME imprecision). So the kvmclock should be
	 * offset from CLOCK_MONOTONIC_RAW by exactly:
	 *   (original delta) + tsc_ofs_delta_ns
	 *
	 * The "original delta" has uncertainty from the source ABA spread,
	 * and the measurement has uncertainty from the destination ABA spread.
	 * Verify the expected value falls within the combined bounds.
	 */
	int64_t delta_before_lo = (int64_t)(kvm_before - src_mono_hi);
	int64_t delta_before_hi = (int64_t)(kvm_before - src_mono_lo);
	int64_t expected_lo = delta_before_lo + tsc_ofs_delta_ns;
	int64_t expected_hi = delta_before_hi + tsc_ofs_delta_ns;
	int64_t actual_lo = (int64_t)(kvm_after - mono_hi);
	int64_t actual_hi = (int64_t)(kvm_after - mono_lo);

	/* Show the shift relative to the source measurement */
	int64_t expected_mid = tsc_ofs_delta_ns;
	int64_t expected_err = (int64_t)(src_mono_hi - src_mono_lo) / 2;
	int64_t actual_mid = ((actual_lo + actual_hi) / 2) - delta_before;
	int64_t actual_err = (int64_t)(mono_hi - mono_lo) / 2;
	pr_info("  kvmclock-mono shift: expected %" PRId64 " ns (±%" PRId64
		"), measured %" PRId64 " ns (±%" PRId64 ")\n",
		expected_mid, expected_err, actual_mid, actual_err);

	/* The ranges must overlap */
	TEST_ASSERT(expected_hi >= actual_lo && expected_lo <= actual_hi,
		    "Ranges don't overlap: expected [%" PRId64 ", %" PRId64
		    "] measured [%" PRId64 ", %" PRId64 "]",
		    expected_lo, expected_hi, actual_lo, actual_hi);

	/*
	 * Direct pvclock verification: read the destination pvclock page
	 * and verify that computing kvmclock from pvti_src and pvti_dst
	 * at the same guest TSC gives the same result.
	 *
	 * Get an atomic {host_tsc, kvmclock} pair, scale host_tsc to
	 * guest TSC using KVM_VCPU_TSC_SCALE, then compute kvmclock
	 * from both pvclock structs.
	 */
	struct kvm_clock_data clock_now = {};
	vm_ioctl(vm, KVM_GET_CLOCK, &clock_now);

	struct pvclock_vcpu_time_info *pvti_dst = addr_gpa2hva(vm, KVMCLOCK_GPA);
	uint64_t host_tsc_now = clock_now.host_tsc;
	uint64_t guest_tsc_now;

	if (scale_dst.tsc_frac_bits)
		guest_tsc_now = ((unsigned __int128)host_tsc_now *
				 scale_dst.tsc_ratio >> scale_dst.tsc_frac_bits)
				+ ofs_dst;
	else
		guest_tsc_now = host_tsc_now + ofs_dst;

	uint64_t clk_from_src = pvclock_read_cycles(&pvti_src, guest_tsc_now);
	uint64_t clk_from_dst = pvclock_read_cycles(pvti_dst, guest_tsc_now);
	int64_t pvclock_delta = (int64_t)(clk_from_src - clk_from_dst);

	pr_info("  Pvclock direct: src=%" PRIu64 " dst=%" PRIu64
		" delta=%" PRId64 " ns\n", clk_from_src, clk_from_dst, pvclock_delta);
	pr_info("  KVM_GET_CLOCK:  %" PRIu64 " ns\n", (uint64_t)clock_now.clock);

	TEST_ASSERT(pvclock_delta >= -1 && pvclock_delta <= 1,
		    "pvclock src vs dst disagree by %" PRId64 " ns", pvclock_delta);

	/*
	 * Tight ABA: compare pvclock_read() directly (no ioctl) against
	 * CLOCK_MONOTONIC_RAW. The spread should be much smaller since
	 * there's no syscall between the two clock_gettime calls — just
	 * rdtsc + userspace mul/shift.
	 */
	uint64_t tight_mono_lo = 0, tight_mono_hi = 0, tight_kvm = 0;
	uint64_t tight_best_spread = UINT64_MAX;
	for (int i = 0; i < 3; i++) {
		struct timespec ts1, ts2;
		uint64_t m1, m2, tsc, clk;

		clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
		tsc = rdtsc();
		clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);

		m1 = ts1.tv_sec * 1000000000ULL + ts1.tv_nsec;
		m2 = ts2.tv_sec * 1000000000ULL + ts2.tv_nsec;

		/* Scale host TSC to guest TSC */
		if (scale_dst.tsc_frac_bits)
			tsc = ((unsigned __int128)tsc * scale_dst.tsc_ratio
			       >> scale_dst.tsc_frac_bits) + ofs_dst;
		else
			tsc += ofs_dst;

		clk = pvclock_read_cycles(pvti_dst, tsc);

		if (m2 - m1 < tight_best_spread) {
			tight_best_spread = m2 - m1;
			tight_mono_lo = m1;
			tight_mono_hi = m2;
			tight_kvm = clk;
		}
	}
	pr_info("  Tight ABA spread: %" PRIu64 " ns (best of 3)\n", tight_best_spread);

	int64_t tight_expected_lo = delta_before_lo + tsc_ofs_delta_ns;
	int64_t tight_expected_hi = delta_before_hi + tsc_ofs_delta_ns;
	int64_t tight_actual_lo = (int64_t)(tight_kvm - tight_mono_hi);
	int64_t tight_actual_hi = (int64_t)(tight_kvm - tight_mono_lo);
	int64_t tight_actual_mid = ((tight_actual_lo + tight_actual_hi) / 2) - delta_before;
	int64_t tight_actual_err = (int64_t)(tight_mono_hi - tight_mono_lo) / 2;

	pr_info("  Tight kvmclock-mono shift: expected %" PRId64
		" ns (±%" PRId64 "), measured %" PRId64 " ns (±%" PRId64 ")\n",
		expected_mid, expected_err, tight_actual_mid, tight_actual_err);

	TEST_ASSERT(tight_expected_hi >= tight_actual_lo &&
		    tight_expected_lo <= tight_actual_hi,
		    "Tight ABA ranges don't overlap");

	kvm_vm_release(vm);
	pr_info("PASS: kvmclock offset matches TSC delta from CLOCK_REALTIME"
		" (%" PRId64 " ns) within ABA bounds\n", tsc_ofs_delta_ns);
	return 0;
}
