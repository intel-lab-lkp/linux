// SPDX-License-Identifier: GPL-2.0-only
/*
 * VMX tertiary controls test
 *
 * Copyright (C) 2026 Intel Corporation
 */
#include "kvm_util.h"
#include "vmx.h"

static bool has_guest_apic_timer_virt = true;
#define TIMER_VECTOR	0xec

#define L2_GUEST_STACK_SIZE 64
static unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

static void l2_guest_code(void)
{
	vmcall();

	/* L1 stops L2 vcpu at above vmcall(). */
	GUEST_FAIL("L2 should not reach here.");
}

static void setup_l2(struct vmx_pages *vmx_pages)
{
	uint64_t ctls;

	GUEST_ASSERT(prepare_for_vmx_operation(vmx_pages));
	GUEST_ASSERT(load_vmcs(vmx_pages));
	prepare_vmcs(vmx_pages, l2_guest_code,
		     &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	GUEST_ASSERT(!vmread(PIN_BASED_VM_EXEC_CONTROL, &ctls));
	ctls |= PIN_BASED_EXT_INTR_MASK;
	GUEST_ASSERT(!vmwrite(PIN_BASED_VM_EXEC_CONTROL, ctls));

	GUEST_ASSERT(!vmread(CPU_BASED_VM_EXEC_CONTROL, &ctls));
	ctls |= CPU_BASED_USE_MSR_BITMAPS | CPU_BASED_TPR_SHADOW |
		CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;
	ctls |= CPU_BASED_ACTIVATE_TERTIARY_CONTROLS;
	GUEST_ASSERT(!vmwrite(CPU_BASED_VM_EXEC_CONTROL, ctls));

	GUEST_ASSERT(!vmread(SECONDARY_VM_EXEC_CONTROL, &ctls));
	ctls |= SECONDARY_EXEC_VIRTUALIZE_X2APIC_MODE |
		SECONDARY_EXEC_APIC_REGISTER_VIRT |
		SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY;
	GUEST_ASSERT(!vmwrite(SECONDARY_VM_EXEC_CONTROL, ctls));

	GUEST_ASSERT(!vmread(TERTIARY_VM_EXEC_CONTROL, &ctls));
	ctls |= TERTIARY_EXEC_GUEST_APIC_TIMER;
	/*
	 * vmwrite() succeeds even if unsuported bit is set.
	 * follwoing vmenter will fail instead.
	 */
	GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls));

	if (has_guest_apic_timer_virt) {
		GUEST_ASSERT(!vmwrite(GUEST_DEADLINE_VIR, 0));
		GUEST_ASSERT(!vmwrite(GUEST_DEADLINE_PHY, 0));
		GUEST_ASSERT(!vmwrite(GUEST_APIC_TIMER_VECTOR, TIMER_VECTOR));
	} else {
		GUEST_ASSERT(vmwrite(GUEST_DEADLINE_VIR, 0));
		GUEST_ASSERT(vmwrite(GUEST_DEADLINE_PHY, 0));
		GUEST_ASSERT(vmwrite(GUEST_APIC_TIMER_VECTOR, TIMER_VECTOR));
	}
}

static void l1_guest_code(struct vmx_pages *vmx_pages)
{
	/* Prepare the VMCS for L2 execution. */
	setup_l2(vmx_pages);

	/* Run L2 */
	if (!has_guest_apic_timer_virt) {
		uint64_t ctls3;

		GUEST_ASSERT(vmlaunch());

		GUEST_ASSERT(!vmread(TERTIARY_VM_EXEC_CONTROL, &ctls3));
		ctls3 &= ~TERTIARY_EXEC_GUEST_APIC_TIMER;
		GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls3));
	}
	GUEST_ASSERT(!vmlaunch());
	GUEST_ASSERT(vmreadz(VM_EXIT_REASON) == EXIT_REASON_VMCALL);

	GUEST_DONE();
}

static void vmx_tertiary_controls_test(void)
{
	uint64_t ctls, ctls3, r;
	struct kvm_vcpu *vcpu;
	vm_vaddr_t guest_gva;
	struct kvm_vm *vm;
	bool done;

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
	vcpu_alloc_vmx(vm, &guest_gva);
	vcpu_args_set(vcpu, 1, guest_gva);

	ctls = vcpu_get_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
	ctls |= (uint64_t)CPU_BASED_ACTIVATE_TERTIARY_CONTROLS << 32;
	vcpu_set_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS, ctls);

	ctls3 = vcpu_get_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3);
	ctls3 |= TERTIARY_EXEC_GUEST_APIC_TIMER;

	r = _vcpu_set_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, ctls3);
	if (has_guest_apic_timer_virt)
		TEST_ASSERT_MSR(r == 1,
			       "KVM_SET_MSR on %s failed value = 0x%lx",
				MSR_IA32_VMX_PROCBASED_CTLS3,
				"MSR_IA32_VMX_PROCBASED_CTLS3",
				ctls3);
	else {
		TEST_ASSERT_MSR(r != 1,
				"KVM_SET_MSR on %s should fail value = 0x%lx",
				MSR_IA32_VMX_PROCBASED_CTLS3,
				"MSR_IA32_VMX_PROCBASED_CTLS3",
				ctls3);
	}
	sync_global_to_guest(vm, has_guest_apic_timer_virt);

	done = false;
	while (!done) {
		struct ucall uc;

		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_DONE:
			done = true;
			break;
		default:
			TEST_ASSERT(false, "Unknown ucall %lu", uc.cmd);
		}
	}

	kvm_vm_free(vm);
}

int main(void)
{
	union vmx_ctrl_msr ctls;
	uint64_t ctls3;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VMX));

	ctls.val = kvm_get_feature_msr(MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
	TEST_REQUIRE(ctls.clr & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS);

	ctls3 = kvm_get_feature_msr(MSR_IA32_VMX_PROCBASED_CTLS3);
	has_guest_apic_timer_virt = ctls3 & TERTIARY_EXEC_GUEST_APIC_TIMER;

	disable_inkernel_irqchip = false;
	vmx_tertiary_controls_test();

	/* nested apic timer virtualization requires in-kernel apic */
	disable_inkernel_irqchip = true;
	has_guest_apic_timer_virt = false;
	vmx_tertiary_controls_test();

	return 0;
}
