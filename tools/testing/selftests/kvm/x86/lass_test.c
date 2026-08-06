// SPDX-License-Identifier: GPL-2.0
/*
 * Linear Address Space Separation (LASS) test
 *
 * Copyright (C) 2026, Intel Corporation.
 *
 * Only test that the guest can't set CR4.LASS when LASS isn't
 * enumerated in the guest's CPUID.  KVM's handling of CR4.LASS via
 * KVM_SET_SREGS is covered by set_sregs_test.
 *
 * Testing LASS enforcement requires running supervisor code in the
 * upper half of the address space, which the KVM selftests framework
 * doesn't support. Enabling CR4.LASS in the current framework would
 * make the next instruction fetch a violation and triple-fault the
 * guest.
 */
#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

/*
 * Without LASS in CPUID, a guest write must generate #GP without
 * changing CR4. Reserved bits are owned by KVM so the write is
 * guaranteed to exit to KVM.
 */
static void guest_code(void)
{
	u8 vector;

	GUEST_ASSERT(!this_cpu_has(X86_FEATURE_LASS));

	vector = kvm_asm_safe("mov %[cr4], %%cr4",
			      [cr4] "r"(get_cr4() | X86_CR4_LASS));
	__GUEST_ASSERT(vector == GP_VECTOR,
		       "Wanted #GP on CR4.LASS, got %s", ex_str(vector));
	GUEST_ASSERT(!(get_cr4() & X86_CR4_LASS));

	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_LASS));

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	vcpu_clear_cpuid_feature(vcpu, X86_FEATURE_LASS);

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	kvm_vm_free(vm);
	return 0;
}
