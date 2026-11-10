// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test for KVM Exits to userspace on s390x
 *
 * Copyright IBM Corp. 2026
 *
 * Authors:
 *  Nitya Khamar <nkhamar@linux.ibm.com>
 *  Aaron Malik Brown <aaronmbr@linux.ibm.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include "test_util.h"
#include "kvm_util.h"
#include "kselftest.h"
#include <errno.h>
#include "processor.h"
#include "ucall_common.h"
#include "sie.h"
#include "kvm_lifecycle.h"

#define NUM_GPRS 16
#define DIAG_ASSEMBLY_CODE 0x501
#define IPA_DIAG 0x83
#define PSW_MASK_64BIT_DAT_OFF  0x0000000180000000ULL
#define PGM_CODE_SEGMENT_TRANSLATION 0x10
#define PSW_WAIT_BIT_MASK (1ULL << (63 - 14))
#define PGM_CODE_GPA_ADDRESS 0x8e
#define PGM_CODE_MASK        0x7f
#define PGM_NEW_PSW_GPA 0x1d0
#define PSW_DISABLED_WAIT 0x0002000180000000ULL

static struct kvm_parameters create_vm_kvm_exit(struct kvm_parameters parameters)
{
	parameters.vm = vm_create_with_one_vcpu(&parameters.vcpu,
						parameters.guest_code);
	return parameters;
}

static void cleanup_kvm(struct kvm_parameters *parameters)
{
	kvm_vm_free(parameters->vm);
}

static void compare_regs(struct kvm_regs *regs, struct kvm_sync_regs *sregs)
{
	int register_number;

	for (register_number = 0; register_number < NUM_GPRS; register_number++)
		TEST_ASSERT_EQ(regs->gprs[register_number], sregs->gprs[register_number]);
}

static void guestcode_instruction_intercept(void)
{
	asm volatile("diag 0,0, 0x500\n" :::);
	GUEST_DONE();
}

static bool test_instruction_intercept(void)
{
	struct kvm_parameters param = {
		.guest_code = guestcode_instruction_intercept,
		.interrupt_type = KVM_S390_INT_EXTERNAL_CALL,
		.manual_parameters.immediate_exit_condition = false,
	};
	struct kvm_s390_irq_state irq_state = {};
	struct kvm_s390_irq irq = {};
	struct kvm_regs regs;
	int interrupts;

	ksft_print_msg("Testing Instruction Intercept Exit\n");
	param = create_vm_kvm_exit(param);
	vcpu_run(param.vcpu);
	param.run = param.vcpu->run;
	irq.type = param.interrupt_type;
	irq.u.extcall.code = param.vcpu->id;
	irq_state.len = sizeof(irq);
	irq_state.buf = (unsigned long)&irq;
	param.irq = &irq;
	param.irq_state = &irq_state;
	TEST_ASSERT_KVM_EXIT_REASON(param.vcpu, KVM_EXIT_S390_SIEIC);
	TEST_ASSERT(param.run->s390_sieic.icptcode == ICPT_INST,
		    "Failed with Unexpected icptcode of 0x%x\n",
		    param.run->s390_sieic.icptcode);
	interrupts = __vcpu_ioctl(param.vcpu, KVM_S390_SET_IRQ_STATE,
				  param.irq_state);
	TEST_ASSERT(interrupts >= 0, "Cannot Inject Interrupt (%d)", errno);
	irq_state.buf = (unsigned long)&irq;
	irq_state.len = sizeof(irq);
	interrupts = __vcpu_ioctl(param.vcpu, KVM_S390_GET_IRQ_STATE,
				  &irq_state);
	TEST_ASSERT(interrupts > 0,
		    "Expected pending interrupt after injection, got %d", interrupts);
	TEST_ASSERT_EQ(irq.type, KVM_S390_INT_EXTERNAL_CALL);
	TEST_ASSERT_EQ(irq.u.extcall.code, param.vcpu->id);
	vcpu_run(param.vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(param.vcpu, KVM_EXIT_S390_SIEIC);
	TEST_ASSERT(param.run->s390_sieic.icptcode == ICPT_INST,
		    "Expected ICPT_INST from GUEST_DONE after inject, got 0x%x\n",
		    param.run->s390_sieic.icptcode);
	vcpu_regs_get(param.vcpu, &regs);
	compare_regs(&regs, &param.run->s.regs);
	cleanup_kvm(&param);
	return true;
}

static void guestcode_diagnose_hypercall(void)
{
	asm volatile("diag 0,0, 0x501\n" :::);
}

static bool test_diagnose_code(void)
{
	struct kvm_parameters param = {
		.guest_code = guestcode_diagnose_hypercall,
		.manual_parameters.immediate_exit_condition = false,
	};
	ksft_print_msg("Testing Diagnosis Hypercall Exit\n");
	param = create_vm_kvm_exit(param);
	vcpu_run(param.vcpu);
	param.run = param.vcpu->run;
	TEST_ASSERT_KVM_EXIT_REASON(param.vcpu, KVM_EXIT_S390_SIEIC);
	TEST_ASSERT(param.run->s390_sieic.icptcode == ICPT_INST,
		    "Failed with Unexpected Exit Reason of %u and icptcode of 0x%x\n",
		    param.run->exit_reason, param.run->s390_sieic.icptcode);
	TEST_ASSERT(param.run->s390_sieic.ipa >> 8 == IPA_DIAG,
		    "Failed with Unexpected Opcode of %u\n",
		    param.run->s390_sieic.ipa >> 8);
	TEST_ASSERT(param.run->s390_sieic.ipb >> 16 == DIAG_ASSEMBLY_CODE,
		    "Failed with Unexpected Diagnose Code of %u\n",
		    param.run->s390_sieic.ipb >> 16);
	cleanup_kvm(&param);
	return true;
}

static void guestcode_diagnose_conditional(void)
{
	asm volatile("lghi %%r3,0x3333\n"
		"lghi %%r5, 0x555\n"
		"lghi %%r6, 0x3333\n"
		"cgrjne %%r6, %%r3, 1f\n"
		"diag 0,0, 0x501\n"
		"1:\n"
		:
		:
		: "r3", "r5", "r6"
	);
	GUEST_DONE();
}

static bool test_diagnose_conditional_code(void)
{
	struct kvm_parameters param = {
		.guest_code = guestcode_diagnose_conditional,
		.manual_parameters.immediate_exit_condition = false,
	};
	struct kvm_regs regs;

	ksft_print_msg("Testing Diagnosis Hypercall for Conditional Code Exit\n");
	param = create_vm_kvm_exit(param);
	vcpu_run(param.vcpu);
	param.run = param.vcpu->run;
	TEST_ASSERT_KVM_EXIT_REASON(param.vcpu, KVM_EXIT_S390_SIEIC);
	TEST_ASSERT(param.run->s390_sieic.icptcode == ICPT_INST,
		    "Failed with Unexpected Exit Reason of %u and icptcode of 0x%x\n",
		    param.run->exit_reason, param.run->s390_sieic.icptcode);
	TEST_ASSERT(param.run->s390_sieic.ipa >> 8 == IPA_DIAG,
		    "Failed with Unexpected Opcode of %u\n",
		    param.run->s390_sieic.ipa >> 8);
	TEST_ASSERT(param.run->s390_sieic.ipb >> 16 == DIAG_ASSEMBLY_CODE,
		    "Failed with Unexpected Diagnose Code of %u\n",
		    param.run->s390_sieic.ipb >> 16);
	vcpu_regs_get(param.vcpu, &regs);
	TEST_ASSERT_EQ(regs.gprs[3], 0x3333);
	TEST_ASSERT_EQ(regs.gprs[5], 0x555);
	TEST_ASSERT_EQ(regs.gprs[6], 0x3333);
	cleanup_kvm(&param);
	return true;
}

/*
 * Make a program interrupt end in a disabled wait, which KVM reports to
 * userspace as ICPT_WAIT. This does not depend on how KVM handles
 * repeated operation exceptions.
 */
static void set_pgm_new_psw_disabled_wait(struct kvm_vm *vm)
{
	uint64_t *new_psw = addr_gpa2hva(vm, PGM_NEW_PSW_GPA);

	new_psw[0] = PSW_DISABLED_WAIT;
	new_psw[1] = 0;
}

static void guestcode_sigp(void)
{
	asm volatile(
		"lghi %%r3, 0\n"
		"sigp %%r1, %%r3, 0x05\n"
		:
		:
		: "r1", "r3"
	);
	GUEST_DONE();
}

static bool test_sigp_signals(void)
{
	struct kvm_parameters param = {
		.guest_code = guestcode_sigp,
		.manual_parameters.immediate_exit_condition = false,
	};
	struct kvm_mp_state mp_state = { .mp_state = KVM_MP_STATE_OPERATING };
	struct kvm_vcpu *vcpu1;
	struct kvm_regs regs;

	ksft_print_msg("Testing SIGP KVM Exit\n");
	param = create_vm_kvm_exit(param);
	vcpu1 = vm_vcpu_add(param.vm, 1, guestcode_sigp);
	/* New vCPUs are stopped, and KVM ignores a SIGP STOP sent to one */
	vcpu_mp_state_set(param.vcpu, &mp_state);
	vcpu_mp_state_set(vcpu1, &mp_state);
	vcpu_regs_get(param.vcpu, &regs);
	regs.gprs[3] = 0x3333;
	vcpu_regs_set(param.vcpu, &regs);
	vcpu_run(vcpu1);
	/* vcpu1 exits after GUEST_DONE() ucall */
	TEST_ASSERT_KVM_EXIT_REASON(vcpu1, KVM_EXIT_S390_SIEIC);
	vcpu_run(param.vcpu);
	param.run = param.vcpu->run;
	TEST_ASSERT_KVM_EXIT_REASON(param.vcpu, KVM_EXIT_S390_SIEIC);
	TEST_ASSERT_EQ(param.run->s390_sieic.icptcode, ICPT_STOP);
	/*
	 * The pending SIGP STOP stops vcpu0 before it runs any guest code
	 * (the guest would clear r3), so r3 keeps the marker value.
	 */
	TEST_ASSERT_EQ(param.run->s.regs.gprs[3], 0x3333);
	cleanup_kvm(&param);
	return true;
}

static void guestcode_address_exception(void)
{
	asm volatile(
		"lghi %%r1, 0x1\n"
		"sllg %%r1, %%r1, 0x28\n"
		"lg %%r2, 0(%%r1)\n"
		:
		:
		: "r1", "r2"
	);
}

static bool test_address_exceptions(void)
{
	struct kvm_parameters param = {};
	uint16_t pgm_code;

	ksft_print_msg("Testing Address Exception Exit\n");
	param.guest_code = guestcode_address_exception;
	param = create_vm_kvm_exit(param);
	set_pgm_new_psw_disabled_wait(param.vm);
	param.vcpu->run->psw_mask &= PSW_MASK_64BIT_DAT_OFF;
	/* The guest runs with DAT off, so the entry point must be a GPA */
	param.vcpu->run->psw_addr = addr_gva2gpa(param.vm, param.vcpu->run->psw_addr);
	param.vcpu->run->kvm_dirty_regs |= KVM_SYNC_ARCH0;
	vcpu_run(param.vcpu);
	param.run = param.vcpu->run;
	TEST_ASSERT_KVM_EXIT_REASON(param.vcpu, KVM_EXIT_S390_SIEIC);
	TEST_ASSERT_EQ(param.run->s390_sieic.icptcode, ICPT_WAIT);
	pgm_code = *(uint16_t *)addr_gpa2hva(param.vm, PGM_CODE_GPA_ADDRESS);
	TEST_ASSERT((pgm_code & PGM_CODE_MASK) == 0x05,
		    "Expected PGM code 0x05 (addressing), got 0x%x\n", pgm_code);
	cleanup_kvm(&param);
	return true;
}

static void guestcode_translation_exception(void)
{
	asm volatile(
		"lghi %%r1, 0x1\n"
		"sllg %%r1, %%r1, 0x14\n"
		"lg %%r2, 0(%%r1)\n"
		:
		:
		: "r1", "r2"
	);
}

static bool test_translation_exceptions(void)
{
	struct kvm_parameters param = {};
	uint16_t pgm_code;

	ksft_print_msg("Testing Translation Exception KVM Exit\n");
	param.guest_code = guestcode_translation_exception;
	param = create_vm_kvm_exit(param);
	set_pgm_new_psw_disabled_wait(param.vm);
	vcpu_run(param.vcpu);
	param.run = param.vcpu->run;
	TEST_ASSERT_KVM_EXIT_REASON(param.vcpu, KVM_EXIT_S390_SIEIC);
	TEST_ASSERT_EQ(param.run->s390_sieic.icptcode, ICPT_WAIT);
	pgm_code = *(uint16_t *)addr_gpa2hva(param.vm, PGM_CODE_GPA_ADDRESS);
	TEST_ASSERT((pgm_code & PGM_CODE_MASK) == PGM_CODE_SEGMENT_TRANSLATION,
		    "Expected PGM code 0x10 (segment translation), got 0x%x\n", pgm_code);
	cleanup_kvm(&param);
	return true;
}

static uint64_t psw_save[2] __aligned(16);

static void guestcode_wait_state(void)
{
	register uint64_t *psw_ptr asm("r4") = psw_save;

	asm volatile(
		"lghi  %%r3, 0x3333\n"
		"lgrl  %%r7, 1f\n"
		"stg   %%r7, 0(%%r4)\n"
		"xgr   %%r5, %%r5\n"
		"stg   %%r5, 8(%%r4)\n"
		"lpswe 0(%%r4)\n"
		".align 8\n"
		"1: .quad 0x0002000180000000\n"
		:
		: "d"(psw_ptr)
		: "r3", "r5", "r7", "memory"
	);
}

static bool test_wait_state(void)
{
	struct kvm_parameters param = {};
	struct kvm_regs regs;

	ksft_print_msg("Testing Wait State Exit\n");
	param.guest_code = guestcode_wait_state;
	param = create_vm_kvm_exit(param);
	vcpu_run(param.vcpu);
	param.run = param.vcpu->run;
	TEST_ASSERT(param.run->exit_reason == KVM_EXIT_S390_SIEIC,
		    "Expected KVM_EXIT_S390_SIEIC, got %u\n",
		    param.run->exit_reason);
	TEST_ASSERT(param.run->s390_sieic.icptcode == ICPT_WAIT,
		    "Failed with Unexpected Exit Reason of %u and icptcode of 0x%x\n",
		    param.run->exit_reason, param.run->s390_sieic.icptcode);
	vcpu_regs_get(param.vcpu, &regs);
	TEST_ASSERT(regs.gprs[3] == 0x3333,
		    "Expected 0x3333 but got 0x%llx\n", regs.gprs[3]);
	/* Check PSW wait bit (bit 14 from MSB = bit 63-14 from LSB) */
	TEST_ASSERT((param.run->psw_mask & PSW_WAIT_BIT_MASK) != 0,
		    "PSW wait bit not set, PSW Mask = 0x%llx\n",
		    param.run->psw_mask);
	cleanup_kvm(&param);
	return true;
}

static void guestcode_nop(void)
{
	GUEST_DONE();
}

static bool test_vm_create_and_destroy(void)
{
	struct kvm_parameters parameters = {.guest_code = guestcode_nop};

	ksft_print_msg("Testing VM Closure without Running vCPU\n");
	parameters = create_vm_kvm_exit(parameters);
	TEST_ASSERT(parameters.vcpu->run->exit_reason == KVM_EXIT_UNKNOWN,
		    "Unexpected Exit Reason Before vCPU Execution: %u\n",
		    parameters.vcpu->run->exit_reason);
	TEST_ASSERT(parameters.vcpu->run->immediate_exit == 0,
		    "Immediate Exit Unexpected Behavior: %d\n",
		    parameters.vcpu->run->immediate_exit);
	cleanup_kvm(&parameters);
	return true;
}

static void test(void)
{
	int i;
	struct kvm_lifecycle_testdef testlist[] = {
		{
			.name = "Testing Instruction Intercepts",
			.test = test_instruction_intercept,
		},
		{
			.name = "Testing DIAG Code",
			.test = test_diagnose_code,
		},
		{
			.name = "Testing Conditional DIAG Code",
			.test = test_diagnose_conditional_code,
		},
		{
			.name = "Testing SIGP Exits",
			.test = test_sigp_signals,
		},
		{
			.name = "Testing Address Exceptions",
			.test = test_address_exceptions,
		},
		{
			.name = "Testing Translation Exceptions",
			.test = test_translation_exceptions,
		},
		{
			.name = "Testing Wait State",
			.test = test_wait_state,
		},
		{
			.name = "Testing VM Create and Destroy",
			.test = test_vm_create_and_destroy,
		},
	};

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(testlist));

	for (i = 0; i < ARRAY_SIZE(testlist); i++) {
		if (testlist[i].test())
			ksft_test_result_pass("%s\n", testlist[i].name);
		else
			ksft_test_result_skip("%s\n", testlist[i].name);
	}
}

int main(void)
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_USER_MEMORY2));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_SYNC_REGS));
	test();
	ksft_finished();
}
