// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026, Google LLC.
 */
#include "kvm_util.h"
#include "vmx.h"
#include "svm_util.h"
#include "kselftest.h"


#define L2_GUEST_STACK_SIZE 64

#define VMRUN_OPCODE 0x000f01d8

int gp_triggered;

static void guest_gp_handler(struct ex_regs *regs)
{
	unsigned char *insn = (unsigned char *)regs->rip;
	u32 opcode = (insn[0] << 16) | (insn[1] << 8) | insn[2];

	GUEST_ASSERT_EQ(opcode, VMRUN_OPCODE);
	GUEST_ASSERT(!gp_triggered);

	gp_triggered = 1;
	regs->rip += 3; /* Skip over VMRUN */
}

static void l2_guest_code(void)
{
	GUEST_SYNC(1);
	vmcall();
}

static void l1_guest_code(struct svm_test_data *svm, u64 invalid_vmcb12_gpa)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	asm volatile ("vmrun %[invalid_vmcb12_gpa]" :
		      : [invalid_vmcb12_gpa] "a" (invalid_vmcb12_gpa)
		      : "memory");
	GUEST_ASSERT_EQ(gp_triggered, 1);

	run_guest(svm->vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(svm->vmcb->control.exit_code == SVM_EXIT_VMMCALL);
	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_x86_state *state;
	vm_vaddr_t nested_gva = 0;
	struct kvm_vcpu *vcpu;
	uint32_t maxphyaddr;
	u64 max_legal_gpa;
	struct kvm_vm *vm;
	struct ucall uc;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
	vm_install_exception_handler(vcpu->vm, GP_VECTOR, guest_gp_handler);

	/*
	 * Find the max legal GPA that is not backed by a memslot (i.e. cannot
	 * be mapped by KVM).
	 */
	maxphyaddr = kvm_cpuid_property(vcpu->cpuid, X86_PROPERTY_MAX_PHY_ADDR);
	max_legal_gpa = BIT_ULL(maxphyaddr) - PAGE_SIZE;
	vcpu_alloc_svm(vm, &nested_gva);
	vcpu_args_set(vcpu, 2, nested_gva, max_legal_gpa);

	/*
	 * Enter L2 (with a legit vmcb12 GPA), then overwrite vmcb12 GPA with
	 * max_legal_gpa. KVM will fail to map vmcb12 on nested VM-Exit and
	 * cause a shutdown.
	 */
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
	TEST_ASSERT_EQ(uc.args[1], 1);

	state = vcpu_save_state(vcpu);
	state->nested.hdr.svm.vmcb_pa = max_legal_gpa;
	vcpu_load_state(vcpu, state);
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_SHUTDOWN);

	kvm_x86_state_cleanup(state);
	kvm_vm_free(vm);
	return 0;
}
