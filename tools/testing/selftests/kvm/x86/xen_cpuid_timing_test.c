// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test that userspace can correctly populate Xen and generic CPUID
 * timing leaves using KVM_GET_TSC_KHZ and KVM_VCPU_TSC_SCALE.
 *
 * This validates that the removal of KVM's runtime Xen CPUID modification
 * doesn't break guests, because userspace has all the information needed.
 */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#include <asm/pvclock-abi.h>

#define XEN_CPUID_BASE		0x40000100
#define XEN_CPUID_LEAF(n)	(XEN_CPUID_BASE + (n))
#define GENERIC_TIMING_LEAF	0x40000010

/* Values set by host, verified by guest */
static uint32_t expected_tsc_khz;
static uint32_t expected_bus_khz;
static uint32_t expected_tsc_mul;
static int8_t   expected_tsc_shift;
static uint64_t host_khz;

static void guest_code(void)
{
	uint32_t eax, ebx, ecx, edx;

	/* Check generic timing leaf 0x40000010 */
	__cpuid(GENERIC_TIMING_LEAF, 0, &eax, &ebx, &ecx, &edx);
	GUEST_ASSERT_EQ(eax, expected_tsc_khz);
	GUEST_ASSERT_EQ(ebx, expected_bus_khz);

	/* Check Xen leaf 3, sub-leaf 0: ECX = guest TSC frequency */
	__cpuid(XEN_CPUID_LEAF(3), 0, &eax, &ebx, &ecx, &edx);
	GUEST_ASSERT_EQ(ecx, expected_tsc_khz);

	/* Check Xen leaf 3, sub-leaf 1: ECX = mul, EDX = shift */
	__cpuid(XEN_CPUID_LEAF(3), 1, &eax, &ebx, &ecx, &edx);
	GUEST_ASSERT_EQ(ecx, expected_tsc_mul);
	GUEST_ASSERT_EQ((int8_t)edx, expected_tsc_shift);

	GUEST_SYNC(0);
}

static void add_cpuid_entry(struct kvm_vcpu *vcpu, uint32_t function,
			    uint32_t index, uint32_t eax, uint32_t ebx,
			    uint32_t ecx, uint32_t edx)
{
	struct kvm_cpuid2 *cpuid = vcpu->cpuid;
	struct kvm_cpuid_entry2 *entry;
	int n = cpuid->nent;

	vcpu->cpuid = realloc(vcpu->cpuid,
			      sizeof(*cpuid) + (n + 1) * sizeof(*entry));
	cpuid = vcpu->cpuid;
	cpuid->nent = n + 1;

	entry = &cpuid->entries[n];
	memset(entry, 0, sizeof(*entry));
	entry->function = function;
	entry->index = index;
	entry->flags = KVM_CPUID_FLAG_SIGNIFCANT_INDEX;
	entry->eax = eax;
	entry->ebx = ebx;
	entry->ecx = ecx;
	entry->edx = edx;
}

/*
 * Compute pvclock mul/shift from frequency, matching kvm_get_time_scale().
 */
static void compute_tsc_mul_shift(uint64_t tsc_hz, uint32_t *mul, int8_t *shift)
{
	uint64_t scaled = 1000000000ULL;
	uint64_t base = tsc_hz;
	int32_t s = 0;
	uint32_t base32;

	while (base > scaled * 2 || base >> 32) {
		base >>= 1;
		s--;
	}
	base32 = (uint32_t)base;
	while (base32 <= scaled || scaled >> 32) {
		if (scaled >> 32 || base32 & (1U << 31))
			scaled >>= 1;
		else
			base32 <<= 1;
		s++;
	}
	*mul = (uint32_t)((scaled << 32) / base32);
	*shift = (int8_t)s;
}

static void run_test(uint64_t tsc_khz)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	uint32_t effective_tsc_khz, effective_bus_khz;
	int bus_cycle_ns;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	if (tsc_khz) {
		pr_info("Testing at TSC frequency %lu kHz\n", tsc_khz);
		vcpu_ioctl(vcpu, KVM_SET_TSC_KHZ, (void *)(unsigned long)tsc_khz);
	} else {
		pr_info("Testing at native TSC frequency\n");
	}

	effective_tsc_khz = __vcpu_ioctl(vcpu, KVM_GET_TSC_KHZ, NULL);
	bus_cycle_ns = vm_check_cap(vm, KVM_CAP_X86_APIC_BUS_CYCLES_NS);
	effective_bus_khz = bus_cycle_ns > 0 ? 1000000 / bus_cycle_ns : 1000000;

	/* If scaling wasn't applied, skip this frequency */
	if (tsc_khz && effective_tsc_khz == host_khz) {
		pr_info("  TSC scaling not available, skipping\n");
		kvm_vm_release(vm);
		return;
	}

	pr_info("  Effective TSC: %u kHz, Bus: %u kHz\n", effective_tsc_khz, effective_bus_khz);

	/* Also exercise KVM_VCPU_TSC_SCALE if available */
	{
		struct { uint64_t ratio; uint64_t frac_bits; } scale;
		struct kvm_device_attr scale_attr = {
			.group = KVM_VCPU_TSC_CTRL,
			.attr = 1, /* KVM_VCPU_TSC_SCALE */
			.addr = (uint64_t)(uintptr_t)&scale,
		};

		if (!__vcpu_ioctl(vcpu, KVM_HAS_DEVICE_ATTR, &scale_attr)) {
			vcpu_ioctl(vcpu, KVM_GET_DEVICE_ATTR, &scale_attr);
			pr_info("  TSC scale: ratio=%lu frac_bits=%lu\n",
				scale.ratio, scale.frac_bits);

			/*
			 * Verify: applying the ratio to the host TSC frequency
			 * should give approximately the effective frequency.
			 */
			if (tsc_khz) {
				uint64_t computed = ((__uint128_t)host_khz * scale.ratio) >> scale.frac_bits;
				int64_t diff = (int64_t)computed - (int64_t)effective_tsc_khz;

				TEST_ASSERT(diff >= -1 && diff <= 1,
					    "TSC_SCALE ratio mismatch: computed %lu vs effective %u (diff %ld)",
					    computed, effective_tsc_khz, diff);
			}
		}
	}

	compute_tsc_mul_shift((uint64_t)effective_tsc_khz * 1000,
			      &expected_tsc_mul, &expected_tsc_shift);

	expected_tsc_khz = effective_tsc_khz;
	expected_bus_khz = effective_bus_khz;

	sync_global_to_guest(vm, expected_tsc_khz);
	sync_global_to_guest(vm, expected_bus_khz);
	sync_global_to_guest(vm, expected_tsc_mul);
	sync_global_to_guest(vm, expected_tsc_shift);

	/* Populate CPUID leaves as a VMM would */
	add_cpuid_entry(vcpu, GENERIC_TIMING_LEAF, 0,
			effective_tsc_khz, effective_bus_khz, 0, 0);
	add_cpuid_entry(vcpu, XEN_CPUID_LEAF(3), 0,
			0, 0, effective_tsc_khz, 0);
	add_cpuid_entry(vcpu, XEN_CPUID_LEAF(3), 1,
			0, 0, expected_tsc_mul,
			(uint32_t)(uint8_t)expected_tsc_shift);

	vcpu_set_cpuid(vcpu);

	pr_info("  pvclock mul=%u shift=%d\n", expected_tsc_mul, expected_tsc_shift);

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

	kvm_vm_release(vm);
}

int main(void)
{
	uint64_t freq;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct kvm_device_attr attr = {
		.group = KVM_VCPU_TSC_CTRL,
		.attr = KVM_VCPU_TSC_SCALE,
	};

	TEST_REQUIRE(sys_clocksource_is_based_on_tsc());

	/* Check KVM_VCPU_TSC_SCALE is supported (implies TSC scaling) */
	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	TEST_REQUIRE(!__vcpu_ioctl(vcpu, KVM_HAS_DEVICE_ATTR, &attr));
	host_khz = __vcpu_ioctl(vcpu, KVM_GET_TSC_KHZ, NULL);
	kvm_vm_release(vm);

	/* Native frequency */
	run_test(0);

	/* Scaled frequencies — skip if TSC scaling not available */
	for (freq = 1000000; freq <= 4000000; freq += 1000000) {
		if (freq == host_khz)
			continue;
		run_test(freq);
	}

	pr_info("PASS: All CPUID timing leaf tests passed\n");
	return 0;
}
