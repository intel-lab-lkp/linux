// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023, Google LLC.
 */
#include <sys/ioctl.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

static void test_hwcr_bit(struct kvm_vcpu *vcpu, unsigned int bit)
{
	const uint64_t ignored = BIT_ULL(3) | BIT_ULL(6) | BIT_ULL(8);
	const uint64_t valid = BIT_ULL(18) | BIT_ULL(24);
	const uint64_t legal = ignored | valid;
	uint64_t val = BIT_ULL(bit);
	uint64_t actual;
	int r;

	r = _vcpu_set_msr(vcpu, MSR_K7_HWCR, val);
	TEST_ASSERT(val & ~legal ? !r : r == 1,
		    "Expected KVM_SET_MSRS(MSR_K7_HWCR) = 0x%lx to %s",
		    val, val & ~legal ? "fail" : "succeed");

	actual = vcpu_get_msr(vcpu, MSR_K7_HWCR);
	TEST_ASSERT(actual == (val & valid),
		    "Bit %u: unexpected HWCR 0x%lx; expected 0x%lx",
		    bit, actual, (val & valid));

	vcpu_set_msr(vcpu, MSR_K7_HWCR, 0);
}

/*
 * AMD-specific: test that HWCR.McStatusWrEn (bit 18) gates guest writes to
 * MCi_STATUS MSRs.  With the bit set, a non-zero write to MC0_STATUS must
 * succeed and read back unchanged.  With the bit clear, the write must take
 * a #GP.
 *
 * This exercises arch/x86/kvm/x86.c:can_set_mci_status(), which is only
 * reachable via the guest WRMSR path (host_initiated=false); test_hwcr_bit()
 * uses KVM_SET_MSRS (host_initiated=true) and never triggers it.
 */
static void guest_code(void)
{
	uint8_t vector;
	uint64_t val;

	/* McStatusWrEn=1: non-zero write to MCi_STATUS must succeed. */
	wrmsr(MSR_K7_HWCR, BIT_ULL(18));
	wrmsr(MSR_IA32_MC0_STATUS, 1);
	val = rdmsr(MSR_IA32_MC0_STATUS);
	GUEST_ASSERT_EQ(val, 1);

	/* Clear the status register before disabling the write-enable bit. */
	wrmsr(MSR_IA32_MC0_STATUS, 0);

	/* McStatusWrEn=0: non-zero write to MCi_STATUS must #GP. */
	wrmsr(MSR_K7_HWCR, 0);
	vector = wrmsr_safe(MSR_IA32_MC0_STATUS, 1);
	GUEST_ASSERT_EQ(vector, GP_VECTOR);

	/* Confirm the failed write left the register at zero. */
	val = rdmsr(MSR_IA32_MC0_STATUS);
	GUEST_ASSERT_EQ(val, 0);

	GUEST_DONE();
}

static void enter_guest(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	while (true) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			return;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_FAIL("Unexpected ucall %lu", uc.cmd);
		}
	}
}

int main(int argc, char *argv[])
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	unsigned int bit;

	vm = vm_create_with_one_vcpu(&vcpu, NULL);

	for (bit = 0; bit < BITS_PER_LONG; bit++)
		test_hwcr_bit(vcpu, bit);

	kvm_vm_free(vm);

	if (host_cpu_is_amd_compatible) {
		vm = vm_create_with_one_vcpu(&vcpu, guest_code);
		enter_guest(vcpu);
		kvm_vm_free(vm);
	}
}
