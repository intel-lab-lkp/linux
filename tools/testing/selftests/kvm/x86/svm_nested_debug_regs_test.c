// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nested SVM debug register state test.
 */
#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"

#define DR6_ACTIVE_LOW	0xffff0ff0
#define DR6_B0		BIT(0)
#define DR6_BS		BIT(14)
#define DR7_FIXED_1	0x400
#define DR7_GE		BIT(9)

#define L1_DR6		(DR6_ACTIVE_LOW | DR6_BS)
#define L2_DR6		(DR6_ACTIVE_LOW | DR6_B0)
#define L1_DR7		(DR7_FIXED_1 | DR7_GE)
#define L2_DR7		(DR7_FIXED_1)

static inline u64 get_dr6(void)
{
	u64 val;

	asm volatile("mov %%dr6, %0" : "=r"(val) : : "memory");
	return val;
}

static inline u64 get_dr7(void)
{
	u64 val;

	asm volatile("mov %%dr7, %0" : "=r"(val) : : "memory");
	return val;
}

static inline void set_dr6(u64 val)
{
	asm volatile("mov %0, %%dr6" : : "r"(val) : "memory");
}

static inline void set_dr7(u64 val)
{
	asm volatile("mov %0, %%dr7" : : "r"(val) : "memory");
}

static void l2_guest_code(void)
{
	GUEST_ASSERT_EQ(get_dr6(), L2_DR6);
	GUEST_ASSERT_EQ(get_dr7(), L2_DR7);
	vmmcall();
}

static void l1_guest_code(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	set_dr6(L1_DR6);
	set_dr7(L1_DR7);

	generic_svm_setup(svm, l2_guest_code);
	vmcb->save.dr6 = L2_DR6;
	vmcb->save.dr7 = L2_DR7;

	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);

	GUEST_ASSERT_EQ(vmcb->save.dr6, L2_DR6);
	GUEST_ASSERT_EQ(vmcb->save.dr7, L2_DR7);
	GUEST_ASSERT_EQ(get_dr6(), L1_DR6);
	GUEST_ASSERT_EQ(get_dr7(), L1_DR7);

	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	gva_t svm_gva;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
	vcpu_alloc_svm(vm, &svm_gva);
	vcpu_args_set(vcpu, 1, svm_gva);

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_DONE:
		break;
	default:
		TEST_FAIL("Unknown ucall %lu", uc.cmd);
	}

	kvm_vm_free(vm);
	return 0;
}
