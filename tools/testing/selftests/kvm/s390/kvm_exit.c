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
