// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026, Google LLC.
 */
#include "kvm_util.h"
#include "vmx.h"
#include "svm_util.h"
#include "kselftest.h"
#include "kvm_test_harness.h"
#include "test_util.h"


#define L2_GUEST_STACK_SIZE 64

#define SYNC_GP 101
#define SYNC_L2_STARTED 102

static unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

static void guest_gp_handler(struct ex_regs *regs)
{
	GUEST_SYNC(SYNC_GP);
}

static void l2_code(void)
{
	GUEST_SYNC(SYNC_L2_STARTED);
	vmcall();
}

static void l1_vmrun(struct svm_test_data *svm, u64 gpa)
{
	generic_svm_setup(svm, l2_code, &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	asm volatile ("vmrun %[gpa]" : : [gpa] "a" (gpa) : "memory");
}

static void l1_vmload(struct svm_test_data *svm, u64 gpa)
{
	generic_svm_setup(svm, l2_code, &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	asm volatile ("vmload %[gpa]" : : [gpa] "a" (gpa) : "memory");
}

static void l1_vmsave(struct svm_test_data *svm, u64 gpa)
{
	generic_svm_setup(svm, l2_code, &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	asm volatile ("vmsave %[gpa]" : : [gpa] "a" (gpa) : "memory");
}

static void l1_vmexit(struct svm_test_data *svm, u64 gpa)
{
	generic_svm_setup(svm, l2_code, &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	run_guest(svm->vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(svm->vmcb->control.exit_code == SVM_EXIT_VMMCALL);
	GUEST_DONE();
}

/*
 * Find the max legal GPA that is not backed by a memslot (i.e. cannot be mapped
 * by KVM).
 */
static u64 unmappable_gpa(struct kvm_vcpu *vcpu)
{
	uint32_t maxphyaddr;
	u64 max_legal_gpa;

	maxphyaddr = kvm_cpuid_property(vcpu->cpuid, X86_PROPERTY_MAX_PHY_ADDR);
	max_legal_gpa = BIT_ULL(maxphyaddr) - PAGE_SIZE;
	TEST_ASSERT(!addr_gpa_has_hva(vcpu->vm, max_legal_gpa),
		    "Expected max_legal_gpa to not be mapped by userspace");

	return max_legal_gpa;
}

static void test_invalid_vmcb12(struct kvm_vcpu *vcpu)
{
	vm_vaddr_t nested_gva = 0;
	struct ucall uc;


	vm_install_exception_handler(vcpu->vm, GP_VECTOR, guest_gp_handler);
	vcpu_alloc_svm(vcpu->vm, &nested_gva);
	vcpu_args_set(vcpu, 2, nested_gva, -1ULL);
	vcpu_run(vcpu);

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
	TEST_ASSERT_EQ(uc.args[1], SYNC_GP);
}

static void test_unmappable_vmcb12(struct kvm_vcpu *vcpu)
{
	vm_vaddr_t nested_gva = 0;

	vcpu_alloc_svm(vcpu->vm, &nested_gva);
	vcpu_args_set(vcpu, 2, nested_gva, unmappable_gpa(vcpu));
	vcpu_run(vcpu);

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_INTERNAL_ERROR);
	TEST_ASSERT_EQ(vcpu->run->emulation_failure.suberror, KVM_INTERNAL_ERROR_EMULATION);
}

static void test_unmappable_vmcb12_vmexit(struct kvm_vcpu *vcpu)
{
	struct kvm_x86_state *state;
	vm_vaddr_t nested_gva = 0;
	struct ucall uc;

	/*
	 * Enter L2 (with a legit vmcb12 GPA), then overwrite vmcb12 GPA with an
	 * unmappable GPA. KVM will fail to map vmcb12 on nested VM-Exit and
	 * cause a shutdown.
	 */
	vcpu_alloc_svm(vcpu->vm, &nested_gva);
	vcpu_args_set(vcpu, 2, nested_gva, unmappable_gpa(vcpu));
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
	TEST_ASSERT_EQ(uc.args[1], SYNC_L2_STARTED);

	state = vcpu_save_state(vcpu);
	state->nested.hdr.svm.vmcb_pa = unmappable_gpa(vcpu);
	vcpu_load_state(vcpu, state);
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_SHUTDOWN);

	kvm_x86_state_cleanup(state);
}

KVM_ONE_VCPU_TEST_SUITE(vmcb12_gpa);

KVM_ONE_VCPU_TEST(vmcb12_gpa, vmrun_invalid, l1_vmrun)
{
	test_invalid_vmcb12(vcpu);
}

KVM_ONE_VCPU_TEST(vmcb12_gpa, vmload_invalid, l1_vmload)
{
	test_invalid_vmcb12(vcpu);
}

KVM_ONE_VCPU_TEST(vmcb12_gpa, vmsave_invalid, l1_vmsave)
{
	test_invalid_vmcb12(vcpu);
}

KVM_ONE_VCPU_TEST(vmcb12_gpa, vmrun_unmappable, l1_vmrun)
{
	test_unmappable_vmcb12(vcpu);
}

KVM_ONE_VCPU_TEST(vmcb12_gpa, vmload_unmappable, l1_vmload)
{
	test_unmappable_vmcb12(vcpu);
}

KVM_ONE_VCPU_TEST(vmcb12_gpa, vmsave_unmappable, l1_vmsave)
{
	test_unmappable_vmcb12(vcpu);
}

/*
 * Invalid vmcb12_gpa cannot be test for #VMEXIT as KVM_SET_NESTED_STATE will
 * reject it.
 */
KVM_ONE_VCPU_TEST(vmcb12_gpa, vmexit_unmappable, l1_vmexit)
{
	test_unmappable_vmcb12_vmexit(vcpu);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));

	return test_harness_run(argc, argv);
}
