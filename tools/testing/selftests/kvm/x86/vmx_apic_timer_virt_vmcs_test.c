// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Intel Corporation
 *
 * Tested vmread()/vmwrite() related to APIC timer virtualization in L1
 * emulated by KVM.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "vmx.h"

#include <string.h>
#include <sys/ioctl.h>
#include <stdatomic.h>

bool have_procbased_tertiary_ctls;
bool have_apic_timer_virtualization;

#define L2_GUEST_STACK_SIZE 256
static unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

/* Any value [32, 255] for timer vector is okay. */
#define TIMER_VECTOR   0xec

static bool update_l2_tsc_deadline;
static uint64_t l2_tsc_deadline;

static void guest_timer_interrupt_handler(struct ex_regs *regs)
{
	x2apic_write_reg(APIC_EOI, 0);
}

static void l2_guest_code(void)
{
	cli();
	x2apic_enable();
	wrmsr(MSR_IA32_TSC_DEADLINE, 0);
	x2apic_write_reg(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | TIMER_VECTOR);

	vmcall();

	while (true) {
		/* reap pending timer interrupt. */
		sti_nop_cli();

		if (update_l2_tsc_deadline)
			GUEST_ASSERT(!wrmsr_safe(MSR_IA32_TSC_DEADLINE, l2_tsc_deadline));

		vmcall();
	}

	GUEST_FAIL("should not reached.");
}

static void setup_l2(struct vmx_pages *vmx_pages)
{
	uint64_t ctls, msr_val;
	int r;

	GUEST_ASSERT(!rdmsr_safe(MSR_IA32_VMX_TRUE_PROCBASED_CTLS,
				 &msr_val));
	GUEST_ASSERT_EQ(have_procbased_tertiary_ctls,
			!!((msr_val >> 32) & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS));

	r = rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS3, &msr_val);
	GUEST_ASSERT(have_procbased_tertiary_ctls == !r);
	if (r)
		msr_val = 0;
	GUEST_ASSERT_EQ(have_apic_timer_virtualization,
			!!(msr_val & TERTIARY_EXEC_GUEST_APIC_TIMER));

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
	if (have_procbased_tertiary_ctls)
		ctls |= CPU_BASED_ACTIVATE_TERTIARY_CONTROLS;
	GUEST_ASSERT(!vmwrite(CPU_BASED_VM_EXEC_CONTROL, ctls));

	GUEST_ASSERT(!vmread(SECONDARY_VM_EXEC_CONTROL, &ctls));
	ctls |= SECONDARY_EXEC_VIRTUALIZE_X2APIC_MODE |
		SECONDARY_EXEC_APIC_REGISTER_VIRT |
		SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY;
	GUEST_ASSERT(!vmwrite(SECONDARY_VM_EXEC_CONTROL, ctls));

	if (have_procbased_tertiary_ctls) {
		GUEST_ASSERT(!vmread(TERTIARY_VM_EXEC_CONTROL, &ctls));

		ctls &= ~TERTIARY_EXEC_GUEST_APIC_TIMER;
		GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls));
	} else {
		GUEST_ASSERT(vmread(TERTIARY_VM_EXEC_CONTROL, &ctls));
		ctls = 0;
	}

	ctls |= TERTIARY_EXEC_GUEST_APIC_TIMER;
	GUEST_ASSERT_EQ(have_procbased_tertiary_ctls,
			!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls));
	if (have_procbased_tertiary_ctls && !have_apic_timer_virtualization) {
		ctls &= ~TERTIARY_EXEC_GUEST_APIC_TIMER;
		GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls));
	}

	GUEST_ASSERT_EQ(have_apic_timer_virtualization,
			!vmwrite(GUEST_APIC_TIMER_VECTOR, TIMER_VECTOR));
}

static void skip_guest_instruction(void)
{
	uint64_t guest_rip, length;

	GUEST_ASSERT(!vmread(GUEST_RIP, &guest_rip));
	GUEST_ASSERT(!vmread(VM_EXIT_INSTRUCTION_LEN, &length));

	GUEST_ASSERT(!vmwrite(GUEST_RIP, guest_rip + length));
	GUEST_ASSERT(!vmwrite(VM_EXIT_INSTRUCTION_LEN, 0));
}

static void l2_load_vmlaunch(struct vmx_pages *vmx_pages)
{
	GUEST_ASSERT(load_vmcs(vmx_pages));
	skip_guest_instruction();
	GUEST_ASSERT(!vmlaunch());
	GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);
}

struct vmcs_guest_deadline {
	uint64_t vir;
	uint64_t phy;
};

struct deadline_test {
	struct vmcs_guest_deadline set;
	struct vmcs_guest_deadline result;
};

static void test_vmclear_vmptrld(struct vmx_pages *vmx_pages)
{
	struct deadline_test tests[] = {
		{
			.set = {
				.vir = ~0ull,
				.phy = ~0ull,
			},
			.result = {
				.vir = ~0ull,
				.phy = ~0ull,
			}
		},
		{
			.set = {
				.vir = ~0ull,
				.phy = 0,
			},
			.result = {
				.vir = ~0ull,
				.phy = 0,
			}
		},
		{
			.set = {
				.vir = ~0ull,
				.phy = 1,
			},
			.result = {
				.vir = 0,
				.phy = 0,
			}
		},
		{
			.set = {
				.vir = 0,
				.phy = ~0ull,
			},
			.result = {
				.vir = 0,
				.phy = ~0ull,
			}
		},
		{
			.set = {
				.vir = 1,
				.phy = ~0ull,
			},
			.result = {
				.vir = 1,
				.phy = ~0ull,
			}
		},
		{
			.set = {
				.vir = 1,
				.phy = 1,
			},
			.result = {
				.vir = 0,
				.phy = 0,
			}
		},
		{
			.set = {
				.vir = 1,
				.phy = 0,
			},
			.result = {
				.vir = 1,
				.phy = 0,
			}
		},
		{
			.set = {
				.vir = 0,
				.phy = 1,
			},
			.result = {
				.vir = 0,
				.phy = 0,
			}
		},
	};
	int i;

	if (!have_apic_timer_virtualization)
		return;

	update_l2_tsc_deadline = false;

	/*
	 * Test if KVM properly store/load TIMER_VECTOR, guest deadline of
	 * vmcs area to/from memory.
	 * Enforce KVM to store nested vmcs to memory and load it again.
	 * load_vmcs() issues vmclear(), and then vmptrld()
	 */
	l2_load_vmlaunch(vmx_pages);
	GUEST_ASSERT_EQ(vmreadz(GUEST_APIC_TIMER_VECTOR), TIMER_VECTOR);

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		struct deadline_test *test = &tests[i];
		uint64_t vir, phy, val;

		GUEST_ASSERT(!vmwrite(GUEST_DEADLINE_VIR, test->set.vir));
		GUEST_ASSERT(!vmread(GUEST_DEADLINE_VIR, &val));
		GUEST_ASSERT_EQ(test->set.vir, val);

		GUEST_ASSERT(!vmwrite(GUEST_DEADLINE_PHY, test->set.phy));
		GUEST_ASSERT(!vmread(GUEST_DEADLINE_PHY, &val));
		GUEST_ASSERT_EQ(test->set.phy, val);

		l2_load_vmlaunch(vmx_pages);

		GUEST_ASSERT(!vmread(GUEST_DEADLINE_VIR, &vir));
		GUEST_ASSERT(!vmread(GUEST_DEADLINE_PHY, &phy));

		GUEST_ASSERT_EQ(vir, test->result.vir);
		GUEST_ASSERT_EQ(!!phy, !!test->result.phy);
	}
}

static void test_ctls(void)
{
	uint64_t ctls;

	update_l2_tsc_deadline = false;

	GUEST_ASSERT_EQ(!vmwrite(GUEST_APIC_TIMER_VECTOR, TIMER_VECTOR),
			have_apic_timer_virtualization);
	GUEST_ASSERT_EQ(!vmwrite(GUEST_DEADLINE_VIR, 0),
			have_apic_timer_virtualization);
	GUEST_ASSERT_EQ(!vmwrite(GUEST_DEADLINE_PHY, 0),
			have_apic_timer_virtualization);

	if (!have_procbased_tertiary_ctls) {
		GUEST_ASSERT(!vmread(CPU_BASED_VM_EXEC_CONTROL, &ctls));
		ctls |= CPU_BASED_ACTIVATE_TERTIARY_CONTROLS;
		GUEST_ASSERT(!vmwrite(CPU_BASED_VM_EXEC_CONTROL, ctls));

		skip_guest_instruction();
		GUEST_ASSERT(vmresume());

		ctls &= ~CPU_BASED_ACTIVATE_TERTIARY_CONTROLS;
		GUEST_ASSERT(!vmwrite(CPU_BASED_VM_EXEC_CONTROL, ctls));
	}

	if (have_procbased_tertiary_ctls && !have_apic_timer_virtualization) {
		GUEST_ASSERT(!vmread(TERTIARY_VM_EXEC_CONTROL, &ctls));
		ctls |= TERTIARY_EXEC_GUEST_APIC_TIMER;
		GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls));

		skip_guest_instruction();
		GUEST_ASSERT(vmresume());

		ctls &= ~TERTIARY_EXEC_GUEST_APIC_TIMER;
		GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls));
	}
}

static void test_l2_set_deadline(void)
{
	uint64_t deadlines[] = {
		0,
		1,
		2,

		rdtsc() / 2,
		rdtsc(),
		rdtsc() * 2,

		~0ull / 2 - 1,
		~0ull / 2,
		~0ull / 2 + 1,

		~0ull - 1,
		~0ull - 2,
		~0ull,
	};
	int i;

	update_l2_tsc_deadline = true;

	for (i = 0; i < ARRAY_SIZE(deadlines); i++) {
		uint64_t phy, vir;

		l2_tsc_deadline = deadlines[i];

		skip_guest_instruction();
		GUEST_ASSERT(!vmresume());
		GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);

		GUEST_ASSERT(!vmread(GUEST_DEADLINE_VIR, &vir));
		GUEST_ASSERT(!vmread(GUEST_DEADLINE_PHY, &phy));

		GUEST_ASSERT(!vir || vir == l2_tsc_deadline);
	}
}

static void l1_guest_code(struct vmx_pages *vmx_pages)
{
	setup_l2(vmx_pages);

	GUEST_ASSERT_EQ(have_apic_timer_virtualization,
			!vmwrite(GUEST_DEADLINE_VIR, ~0ull));
	GUEST_ASSERT_EQ(have_apic_timer_virtualization,
			!vmwrite(GUEST_DEADLINE_PHY, ~0ull));

	test_vmclear_vmptrld(vmx_pages);

	update_l2_tsc_deadline = false;
	l2_load_vmlaunch(vmx_pages);
	test_ctls();

	if (have_apic_timer_virtualization) {
		update_l2_tsc_deadline = false;
		l2_load_vmlaunch(vmx_pages);

		test_l2_set_deadline();
	}

	GUEST_DONE();
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	bool done = false;

	while (!done) {
		struct ucall uc;

		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_PRINTF:
			pr_info("%s", uc.buffer);
			break;
		case UCALL_DONE:
			done = true;
			break;
		default:
			TEST_ASSERT(false, "Unknown ucall %lu", uc.cmd);
		}
	}
}

static void test_apic_virtualization_vmcs(void)
{
	vm_vaddr_t vmx_pages_gva;

	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	uint64_t ctls, ctls3;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VMX));

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
	vcpu_alloc_vmx(vm, &vmx_pages_gva);
	vcpu_args_set(vcpu, 1, vmx_pages_gva);

	vcpu_set_cpuid_feature(vcpu, X86_FEATURE_TSC_DEADLINE_TIMER);

	ctls = vcpu_get_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
	if (have_procbased_tertiary_ctls) {
		ctls |= (uint64_t)CPU_BASED_ACTIVATE_TERTIARY_CONTROLS << 32;
		vcpu_set_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS, ctls);

		ctls3 = vcpu_get_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3);
		if (have_apic_timer_virtualization)
			ctls3 |= TERTIARY_EXEC_GUEST_APIC_TIMER;
		else
			ctls3 &= ~TERTIARY_EXEC_GUEST_APIC_TIMER;
		vcpu_set_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, ctls3);
	} else {
		ctls &= ~((uint64_t)CPU_BASED_ACTIVATE_TERTIARY_CONTROLS << 32);
		vcpu_set_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS, ctls);
	}

	/* For L2. */
	vm_install_exception_handler(vm, TIMER_VECTOR,
				     guest_timer_interrupt_handler);

	sync_global_to_guest(vm, have_procbased_tertiary_ctls);
	sync_global_to_guest(vm, have_apic_timer_virtualization);
	run_vcpu(vcpu);

	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	have_procbased_tertiary_ctls =
		(kvm_get_feature_msr(MSR_IA32_VMX_TRUE_PROCBASED_CTLS) >> 32) &
		CPU_BASED_ACTIVATE_TERTIARY_CONTROLS;
	have_apic_timer_virtualization = have_procbased_tertiary_ctls &&
		(kvm_get_feature_msr(MSR_IA32_VMX_PROCBASED_CTLS3) &
		 TERTIARY_EXEC_GUEST_APIC_TIMER);

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VMX));

	test_apic_virtualization_vmcs();

	if (have_apic_timer_virtualization) {
		have_apic_timer_virtualization = false;
		test_apic_virtualization_vmcs();
	}

	if (have_procbased_tertiary_ctls) {
		have_procbased_tertiary_ctls = false;
		test_apic_virtualization_vmcs();
	}

	return 0;
}
