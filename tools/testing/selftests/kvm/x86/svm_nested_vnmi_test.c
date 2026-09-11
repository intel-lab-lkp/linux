// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test KVM's virtual NMI (vNMI) support for nested guests: the consistency
 * check on the vNMI controls in vmcb12, delivery of a virtual NMI to L2, and
 * the V_NMI_PENDING/V_NMI_BLOCKING state L1 observes across the NMI.
 *
 * Copyright (C) 2026 Hemanth Selam <hemanth.selam@gmail.com>
 */
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"
#include "test_util.h"

/* Number of virtual NMIs to deliver; more than one to prove it's repeatable. */
#define NR_VNMIS	3

static unsigned int nmi_fired;

static void guest_nmi_handler(struct ex_regs *regs)
{
	nmi_fired++;

	/*
	 * Exit to L1 from NMI context, i.e. before this handler's IRET, so
	 * that L1 can observe V_NMI_BLOCKING while the NMI is in service.
	 */
	vmmcall();
}

static void l2_guest_code(void)
{
	vmmcall();
}

static void l1_vnmi_setup(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	generic_svm_setup(svm, l2_guest_code);

	/* KVM requires L1 to intercept NMIs in order to enable vNMI. */
	vmcb->control.intercept |= BIT(INTERCEPT_NMI);
	vmcb->control.int_ctl |= V_NMI_ENABLE_MASK;
}

static void l1_guest_code(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;
	unsigned int i;

	/* Without a pending virtual NMI, L2 runs to its VMMCALL untouched. */
	l1_vnmi_setup(svm);

	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT_EQ(nmi_fired, 0);
	GUEST_ASSERT(!(vmcb->control.int_ctl & V_NMI_PENDING_MASK));
	GUEST_ASSERT(!(vmcb->control.int_ctl & V_NMI_BLOCKING_MASK));

	for (i = 1; i <= NR_VNMIS; i++) {
		/* Request a virtual NMI; L2 must take it on VMRUN. */
		l1_vnmi_setup(svm);
		vmcb->control.int_ctl |= V_NMI_PENDING_MASK;

		run_guest(vmcb, svm->vmcb_gpa);

		/*
		 * The NMI was delivered and L2 exited from the handler, so
		 * hardware has consumed the request and blocked further NMIs.
		 */
		GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
		GUEST_ASSERT_EQ(nmi_fired, i);
		GUEST_ASSERT(!(vmcb->control.int_ctl & V_NMI_PENDING_MASK));
		GUEST_ASSERT(vmcb->control.int_ctl & V_NMI_BLOCKING_MASK);

		/* Resume L2 so the handler can IRET, which unblocks NMIs. */
		vmcb->save.rip = vmcb->control.next_rip;

		run_guest(vmcb, svm->vmcb_gpa);
		GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
		GUEST_ASSERT_EQ(nmi_fired, i);
		GUEST_ASSERT(!(vmcb->control.int_ctl & V_NMI_BLOCKING_MASK));
	}

	/*
	 * Enabling vNMI without intercepting NMIs is illegal, as L1 would have
	 * no way of observing the NMIs it is nominally responsible for.
	 */
	vmcb->control.intercept &= ~BIT(INTERCEPT_NMI);

	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_ERR);

	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	gva_t svm_gva;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VNMI));

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);

	vm_install_exception_handler(vm, NMI_VECTOR, guest_nmi_handler);

	vcpu_alloc_svm(vm, &svm_gva);
	vcpu_args_set(vcpu, 1, svm_gva);

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		/* NOT REACHED */
	case UCALL_DONE:
		break;
	default:
		TEST_FAIL("Unexpected ucall: %lu", uc.cmd);
	}

	kvm_vm_free(vm);
	return 0;
}
