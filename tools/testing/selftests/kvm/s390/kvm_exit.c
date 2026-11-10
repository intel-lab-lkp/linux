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
#include <linux/capability.h>
#include "test_util.h"
#include "kvm_util.h"
#include "kselftest.h"
#include <errno.h>
#include "processor.h"
#include "ucall_common.h"
#include "sie.h"
#include "kvm_lifecycle.h"
#include <linux/sizes.h>

/* capget() is used to check CAP_SYS_ADMIN without a libcap dependency */
int capget(cap_user_header_t header, cap_user_data_t data);

#define NUM_GPRS 16
#define DIAG_ASSEMBLY_CODE 0x501
#define IPA_DIAG 0x83
#define PSW_MASK_64BIT_DAT_OFF  0x0000000180000000ULL
#define PGM_CODE_SEGMENT_TRANSLATION 0x10
#define CODE_GPA (3 * SZ_1M)
#define MEM_SIZE (4 * SZ_1M)
#define STSI_IPA 0xb27d
#define TSCH_IPA 0xb235
#define STSCH_IPA 0xb234
#define EXPECTED_STORAGE_KEY 0x20
#define IPA_SUBOPCODE_MASK_SKEY 0xff
#define SSKE_SUBOPCODE 0x2b
#define SSKE_OPCODE 0xb2
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

static struct kvm_parameters manual_kvm_creation_execution(struct kvm_parameters param)
{
	int rc;

	param.manual_parameters.kvm_fd = open_kvm_dev_path_or_exit();
	param.manual_parameters.vm_fd = ioctl(param.manual_parameters.kvm_fd,
					      KVM_CREATE_VM, KVM_VM_S390_UCONTROL);

	TEST_ASSERT(param.manual_parameters.vm_fd >= 0, "KVM_CREATE_VM failed (%d)", errno);
	param.manual_parameters.vcpu_fd = ioctl(param.manual_parameters.vm_fd, KVM_CREATE_VCPU, 0);
	TEST_ASSERT(param.manual_parameters.vcpu_fd >= 0, "KVM_CREATE_VCPU failed (%d)", errno);

	param.manual_parameters.vm_mem = aligned_alloc(SZ_1M, MEM_SIZE);
	TEST_ASSERT(param.manual_parameters.vm_mem, "aligned_alloc failed (%d)", errno);

	param.manual_parameters.map.user_addr = (uintptr_t)param.manual_parameters.vm_mem;
	param.manual_parameters.map.vcpu_addr = 0;
	param.manual_parameters.map.length = MEM_SIZE;
	rc = ioctl(param.manual_parameters.vcpu_fd, KVM_S390_UCAS_MAP,
		   &param.manual_parameters.map);
	TEST_ASSERT(rc == 0, "KVM_S390_UCAS_MAP failed (%d)", errno);

	rc = ioctl(param.manual_parameters.vcpu_fd, KVM_S390_VCPU_FAULT, (void *)0);
	TEST_ASSERT(rc == 0, "KVM_S390_VCPU_FAULT failed (%d)", errno);

	/* Copy guest code to physical GPA */
	memcpy((void *)((uintptr_t)param.manual_parameters.vm_mem + CODE_GPA),
	       param.guest_code, PAGE_SIZE);

	/* Map kvm_run */
	param.manual_parameters.run_size = ioctl(param.manual_parameters.kvm_fd,
						 KVM_GET_VCPU_MMAP_SIZE, NULL);
	TEST_ASSERT(param.manual_parameters.run_size >= (int)sizeof(struct kvm_run),
		    "KVM_GET_VCPU_MMAP_SIZE returned (%d) expected >= %zu",
		    param.manual_parameters.run_size, sizeof(struct kvm_run));
	param.run = mmap(NULL, param.manual_parameters.run_size,
			 PROT_READ | PROT_WRITE, MAP_SHARED,
			 param.manual_parameters.vcpu_fd, 0);
	TEST_ASSERT(param.run != MAP_FAILED, "mmap kvm_run failed (%d)", errno);

	/* Map SIE block and clear STOPPED flag - required for ucontrol VMs */
	param.manual_parameters.sie_block = mmap(NULL, PAGE_SIZE,
						 PROT_READ | PROT_WRITE, MAP_SHARED,
						 param.manual_parameters.vcpu_fd,
						 KVM_S390_SIE_PAGE_OFFSET << PAGE_SHIFT);
	TEST_ASSERT(param.manual_parameters.sie_block != MAP_FAILED,
		    "mmap sie_block failed (%d)", errno);
	param.manual_parameters.sie_block->cpuflags &= ~CPUSTAT_STOPPED;

	/* DAT off + 64-bit mode; PSW address = physical code GPA */
	param.run->psw_mask = PSW_MASK_64BIT_DAT_OFF;
	param.run->psw_addr = CODE_GPA;
	param.run->kvm_dirty_regs |= KVM_SYNC_ARCH0;

	/* Point r15 at a valid guest stack below the code page */
	param.run->s.regs.gprs[15] = CODE_GPA - 160;
	param.run->kvm_dirty_regs |= KVM_SYNC_GPRS;

	if (param.manual_parameters.immediate_exit_condition) {
		param.run->immediate_exit = 1;
		param.run->s.regs.gprs[3] = 0xdead;
		param.run->kvm_dirty_regs |= KVM_SYNC_GPRS;
		rc = ioctl(param.manual_parameters.vcpu_fd, KVM_RUN, NULL);
		TEST_ASSERT(rc == -1 && errno == EINTR, "KVM_RUN failed (%d)", errno);
		return param;
	}

	rc = ioctl(param.manual_parameters.vcpu_fd, KVM_RUN, NULL);
	TEST_ASSERT(rc == 0, "KVM_RUN failed (%d)", errno);

	return param;
}

static void manual_cleanup(struct kvm_parameters param)
{
	munmap(param.manual_parameters.sie_block, PAGE_SIZE);
	munmap(param.run, param.manual_parameters.run_size);
	close(param.manual_parameters.vcpu_fd);
	close(param.manual_parameters.vm_fd);
	close(param.manual_parameters.kvm_fd);
	free(param.manual_parameters.vm_mem);
}

static bool ucontrol_available(void)
{
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	int rc;

	rc = capget(&hdr, data);
	if (rc < 0)
		return false;
	if (!(data->effective & CAP_TO_MASK(CAP_SYS_ADMIN)))
		return false;
	return kvm_has_cap(KVM_CAP_S390_UCONTROL);
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

/* Tests below require KVM_CAP_S390_UCONTROL and are skipped if absent. */

static void guestcode_system_information_calls(void)
{
	static uint8_t stsi_buf[4096] __aligned(4096);

	register uint8_t *ptr asm("r1") = stsi_buf;

	asm volatile(
		"lghi %%r0, 0x0302\n"
		"stsi 0(%%r1)\n"
		:
		: "d"(ptr)
		: "r0", "memory"
	);
	GUEST_DONE();
}

static bool test_system_information_calls(void)
{
	struct kvm_parameters parameters = {
		.guest_code = guestcode_system_information_calls,
		.manual_parameters.immediate_exit_condition = false,
	};

	if (!ucontrol_available())
		return false;

	ksft_print_msg("Testing System Information Call Exits\n");

	parameters = manual_kvm_creation_execution(parameters);

	TEST_ASSERT(parameters.run->exit_reason == KVM_EXIT_S390_SIEIC,
		    "Expected KVM_EXIT_S390_SIEIC, got %u\n", parameters.run->exit_reason);
	TEST_ASSERT(parameters.run->s390_sieic.icptcode == ICPT_INST,
		    "Failed with icptcode of 0x%x\n", parameters.run->s390_sieic.icptcode);
	TEST_ASSERT(parameters.run->s390_sieic.ipa == STSI_IPA,
		    "Expected STSI opcode 0xb27d, got 0x%x\n", parameters.run->s390_sieic.ipa);
	manual_cleanup(parameters);
	return true;
}

static void guestcode_tsch(void)
{
	register uint64_t buf asm("r2") = 0x3000;

	asm volatile(
		"lgfi  %%r1, 0x00010000\n"
		"tsch  0(%%r2)\n"
		:
		: "d"(buf)
		: "r1"
	);
}

static void guestcode_stsch(void)
{
	register uint64_t buffer asm("r2") = 0x3000;

	asm volatile(
		"lgfi  %%r1, 0x00010000\n"
		"stsch 0(%%r2)\n"
		:
		: "d"(buffer)
		: "r1"
	);
}

static const struct {
	void (*guest_code)(void);
	uint16_t expected_ipa;
} io_tests[] = {
	{guestcode_tsch, TSCH_IPA},
	{guestcode_stsch, STSCH_IPA},
};

static bool test_io_instruction_code(void)
{
	int i;

	if (!ucontrol_available())
		return false;

	ksft_print_msg("Testing IO Instruction Exit for TSCH and STSCH\n");
	for (i = 0; i < ARRAY_SIZE(io_tests); i++) {
		struct kvm_parameters parameter = {
			.guest_code = io_tests[i].guest_code,
			.manual_parameters.immediate_exit_condition = false,
		};

		parameter = manual_kvm_creation_execution(parameter);

		TEST_ASSERT(parameter.run->exit_reason == KVM_EXIT_S390_SIEIC,
			    "Expected KVM_EXIT_S390_SIEIC, got %u\n",
			    parameter.run->exit_reason);
		TEST_ASSERT(parameter.run->s390_sieic.icptcode == ICPT_INST,
			    "Failed with icptcode of 0x%x\n",
			    parameter.run->s390_sieic.icptcode);
		TEST_ASSERT(parameter.run->s390_sieic.ipa == io_tests[i].expected_ipa,
			    "Expected opcode 0x%x, got 0x%x\n",
			    io_tests[i].expected_ipa,
			    parameter.run->s390_sieic.ipa);
		manual_cleanup(parameter);
	}
	return true;
}

static void guestcode_storage_key(void)
{
	asm volatile(
		"lghi %%r3, 0\n"
		"lghi %%r2, %[skey]\n"
		"sske %%r2, %%r3\n"
		:
		: [skey] "i" (EXPECTED_STORAGE_KEY)
		: "r3", "r2"
	);
}

static bool test_storage_key_interrupt(void)
{
	struct kvm_parameters parameters = {
		.guest_code = guestcode_storage_key,
		.manual_parameters.immediate_exit_condition = false,
	};

	if (!ucontrol_available())
		return false;

	ksft_print_msg("Testing Storage Key Interrupt KVM Exit\n");
	parameters = manual_kvm_creation_execution(parameters);

	/* Skip on hosts where sclp.has_kss is false. */
	if (!(parameters.manual_parameters.sie_block->cpuflags & CPUSTAT_KSS)) {
		manual_cleanup(parameters);
		return false;
	}

	TEST_ASSERT(parameters.run->exit_reason == KVM_EXIT_S390_SIEIC,
		    "Expected KVM_EXIT_S390_SIEIC, got %u\n", parameters.run->exit_reason);
	TEST_ASSERT(parameters.run->s390_sieic.icptcode == ICPT_KSS,
		    "Failed with Unexpected Exit Reason of %u and icptcode of 0x%x\n",
		    parameters.run->exit_reason, parameters.run->s390_sieic.icptcode);
	TEST_ASSERT((parameters.run->s390_sieic.ipa & IPA_SUBOPCODE_MASK_SKEY) == SSKE_SUBOPCODE,
		    "Expected SSKE Sub Opcode of 0x2B, Result of 0x%x\n",
		    parameters.run->s390_sieic.ipa & IPA_SUBOPCODE_MASK_SKEY);
	TEST_ASSERT((parameters.run->s390_sieic.ipa >> 8) == SSKE_OPCODE,
		    "Expected SSKE Opcode of 0xb2, Actual of 0x%x\n",
		    parameters.run->s390_sieic.ipa >> 8);
	TEST_ASSERT(parameters.run->s.regs.gprs[2] == EXPECTED_STORAGE_KEY,
		    "Storage Key not stored correctly, got 0x%llx\n",
		    parameters.run->s.regs.gprs[2]);

	manual_cleanup(parameters);
	return true;
}

static void guestcode_immediate_exit(void)
{
	asm volatile(
		"lghi %%r3, 0x3333\n"
		:
		:
		: "r3"
	);
}

static bool test_immediate_exit(void)
{
	struct kvm_parameters parameters = {
		.guest_code = guestcode_immediate_exit,
		.manual_parameters.immediate_exit_condition = true,
	};

	if (!ucontrol_available())
		return false;

	ksft_print_msg("Testing KVM Immediate Exit\n");
	parameters = manual_kvm_creation_execution(parameters);

	TEST_ASSERT(parameters.run->exit_reason == KVM_EXIT_UNKNOWN,
		    "Expected KVM_EXIT_UNKNOWN (0), got %u\n",
		    parameters.run->exit_reason);
	TEST_ASSERT(parameters.run->s.regs.gprs[3] == 0xdead,
		    "Guest code executed when it should not have: r3=0x%llx\n",
			parameters.run->s.regs.gprs[3]);

	manual_cleanup(parameters);
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
		{
			.name = "Testing System Information Calls",
			.test = test_system_information_calls,
			.skip_reason = "KVM_CAP_S390_UCONTROL unavailable or insufficient privilege"
		},
		{
			.name = "Testing IO Instruction Exits",
			.test = test_io_instruction_code,
			.skip_reason = "KVM_CAP_S390_UCONTROL unavailable or insufficient privilege"
		},
		{
			.name = "Testing Storage Key Interrupts",
			.test = test_storage_key_interrupt,
			.skip_reason = "KVM_CAP_S390_UCONTROL, privilege or KSS unavailable"
		},
		{
			.name = "Testing Immediate Exit",
			.test = test_immediate_exit,
			.skip_reason = "KVM_CAP_S390_UCONTROL unavailable or insufficient privilege"
		},
	};

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(testlist));

	for (i = 0; i < ARRAY_SIZE(testlist); i++) {
		if (testlist[i].test())
			ksft_test_result_pass("%s\n", testlist[i].name);
		else
			ksft_test_result_skip("%s (%s)\n", testlist[i].name,
					      testlist[i].skip_reason ?: "unsupported");
	}
}

int main(void)
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_USER_MEMORY2));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_SYNC_REGS));
	test();
	ksft_finished();
}
