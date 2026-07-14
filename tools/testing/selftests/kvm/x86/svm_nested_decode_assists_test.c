// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test KVM's virtualization of SVM DecodeAssists for nested guests.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"

#define L2_GUEST_STACK_SIZE 64

#define TEST_INT_VECTOR 0x81

/* Any canonical virtual address that is never mapped by the selftest VM. */
#define PF_TEST_GVA BIT_ULL(40)
#define PF_FETCH_TEST_GVA BIT_ULL(41)

#define OUTSB_OPCODE 0x6e
#define INT3_OPCODE 0xcc
#define BOUNDARY_OUTSB_CODE_SIZE 15
#define LIMIT_OUTSB_OPCODE_OFFSET 9
#define USERSPACE_PF_STAGE 1
#define TEST_IOPM_SIZE (3 * PAGE_SIZE)

static u64 npf_target __aligned(PAGE_SIZE);
static u8 boundary_outsb_code[2 * PAGE_SIZE] __aligned(PAGE_SIZE);
static u8 limit_outsb_code[PAGE_SIZE] __aligned(PAGE_SIZE);
static unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

static void l2_read_code(void)
{
	asm volatile("mov (%0), %%rax" : : "r"(&npf_target) : "rax", "memory");
	GUEST_FAIL("L2 read did not cause a nested page fault");
}

static void l2_outsb_code(void)
{
	asm volatile("mov %0, %%rsi\n\t"
		     "mov $0x80, %%dx\n\t"
		     "outsb"
		     : : "r"(&npf_target) : "rsi", "rdx", "memory");
	GUEST_FAIL("L2 OUTSB did not cause a nested page fault");
}

static void l2_pf_code(void)
{
	asm volatile("mov (%0), %%rax"
		     : : "r"(PF_TEST_GVA) : "rax", "memory");
	GUEST_FAIL("L2 access to an unmapped VA did not #PF");
}

static void l2_fep_pf_code(void)
{
	asm volatile(KVM_FEP "mov (%0), %%rax"
		     : : "r"(PF_TEST_GVA) : "rax", "memory");
	GUEST_FAIL("L2 forced-emulated access to an unmapped VA did not #PF");
}

static void l2_userspace_pf_code(void)
{
	GUEST_SYNC(USERSPACE_PF_STAGE);
	GUEST_FAIL("Userspace-injected #PF was not intercepted by L1");
}

static void l2_mov_from_cr4_code(void)
{
	asm volatile("mov %%cr4, %%r10" : : : "r10");
	GUEST_FAIL("L2 MOV-from-CR4 was not intercepted");
}

static void l2_fep_mov_from_cr4_code(void)
{
	asm volatile(KVM_FEP "mov %%cr4, %%r10" : : : "r10");
	GUEST_FAIL("L2 forced-emulated MOV-from-CR4 was not intercepted");
}

static void l2_mov_to_cr4_code(void)
{
	asm volatile("mov %%cr4, %%rax\n\t"
		     "mov %%rax, %%cr4" : : : "rax");
	GUEST_FAIL("L2 MOV-to-CR4 was not intercepted");
}

static void l2_fep_mov_to_cr4_code(void)
{
	asm volatile("mov %%cr4, %%rax\n\t"
		     KVM_FEP "mov %%rax, %%cr4" : : : "rax");
	GUEST_FAIL("L2 forced-emulated MOV-to-CR4 was not intercepted");
}

static void l2_mov_to_dr7_code(void)
{
	asm volatile("mov %%dr7, %%rax\n\t"
		     "mov %%rax, %%rbx\n\t"
		     "mov %%rbx, %%dr7" : : : "rax", "rbx");
	GUEST_FAIL("L2 MOV-to-DR7 was not intercepted");
}

static void l2_fep_mov_to_dr7_code(void)
{
	asm volatile("mov %%dr7, %%rax\n\t"
		     "mov %%rax, %%rbx\n\t"
		     KVM_FEP "mov %%rbx, %%dr7" : : : "rax", "rbx");
	GUEST_FAIL("L2 forced-emulated MOV-to-DR7 was not intercepted");
}

static void l2_mov_from_dr7_code(void)
{
	asm volatile("mov %%dr7, %%r10" : : : "r10");
	GUEST_FAIL("L2 MOV-from-DR7 was not intercepted");
}

static void l2_fep_mov_from_dr7_code(void)
{
	asm volatile(KVM_FEP "mov %%dr7, %%r10" : : : "r10");
	GUEST_FAIL("L2 forced-emulated MOV-from-DR7 was not intercepted");
}

static void l2_clts_code(void)
{
	asm volatile("clts" : : : "memory");
	GUEST_FAIL("L2 CLTS was not intercepted");
}

static void l2_fep_clts_code(void)
{
	asm volatile(KVM_FEP "clts" : : : "memory");
	GUEST_FAIL("L2 forced-emulated CLTS was not intercepted");
}

static void l2_lmsw_code(void)
{
	asm volatile("smsw %%ax\n\t"
		     "lmsw %%ax" : : : "rax", "memory");
	GUEST_FAIL("L2 LMSW was not intercepted");
}

static void l2_fep_lmsw_code(void)
{
	asm volatile("smsw %%ax\n\t"
		     KVM_FEP "lmsw %%ax" : : : "rax", "memory");
	GUEST_FAIL("L2 forced-emulated LMSW was not intercepted");
}

static void l2_smsw_code(void)
{
	asm volatile("smsw %%ax" : : : "rax", "memory");
	GUEST_FAIL("L2 SMSW was not intercepted");
}

static void l2_fep_smsw_code(void)
{
	asm volatile(KVM_FEP "smsw %%ax" : : : "rax", "memory");
	GUEST_FAIL("L2 forced-emulated SMSW was not intercepted");
}

static void l2_int_code(void)
{
	asm volatile("int %0" : : "i"(TEST_INT_VECTOR));
	GUEST_FAIL("L2 INTn was not intercepted");
}

static void l2_fep_int_code(void)
{
	asm volatile(KVM_FEP "int %0" : : "i"(TEST_INT_VECTOR));
	GUEST_FAIL("L2 forced-emulated INTn was not intercepted");
}

static void l2_invlpg_code(void)
{
	asm volatile("invlpg (%0)" : : "r"(&npf_target) : "memory");
	GUEST_FAIL("L2 INVLPG was not intercepted");
}

static void l2_fep_invlpg_code(void)
{
	asm volatile(KVM_FEP "invlpg (%0)" : : "r"(&npf_target) : "memory");
	GUEST_FAIL("L2 forced-emulated INVLPG was not intercepted");
}

static void l2_invlpga_code(void)
{
	asm volatile("invlpga" : : "a"(&npf_target), "c"(0) : "memory");
	GUEST_FAIL("L2 INVLPGA was not intercepted");
}

static void l2_fep_invlpga_code(void)
{
	asm volatile(KVM_FEP "invlpga"
		     : : "a"(&npf_target), "c"(0) : "memory");
	GUEST_FAIL("L2 forced-emulated INVLPGA was not intercepted");
}

static void l2_vmmcall_code(void)
{
	vmmcall();
	GUEST_FAIL("L2 did not exit on VMMCALL");
}

struct instruction_intercept_test {
	const char *name;
	void (*code)(void);
	void (*fep_code)(void);
	u64 intercept;
	u32 intercept_cr;
	u32 intercept_dr;
	u64 exit_code;
	u64 exit_info_1;
	u64 exit_info_1_mask;
	bool check_rax;
	u64 rax;
};

static const struct instruction_intercept_test instruction_intercept_tests[] = {
	{
		.name = "MOV-to-CR4",
		.code = l2_mov_to_cr4_code,
		.fep_code = l2_fep_mov_to_cr4_code,
		.intercept_cr = BIT(INTERCEPT_CR4_WRITE),
		.exit_code = SVM_EXIT_WRITE_CR4,
		.exit_info_1 = BIT_ULL(63),
		.exit_info_1_mask = ~0ULL,
	}, {
		.name = "MOV-from-CR4",
		.code = l2_mov_from_cr4_code,
		.fep_code = l2_fep_mov_from_cr4_code,
		.intercept_cr = BIT(INTERCEPT_CR4_READ),
		.exit_code = SVM_EXIT_READ_CR4,
		.exit_info_1 = BIT_ULL(63) | 10,
		.exit_info_1_mask = ~0ULL,
	}, {
		.name = "MOV-to-DR7",
		.code = l2_mov_to_dr7_code,
		.fep_code = l2_fep_mov_to_dr7_code,
		.intercept_dr = BIT(INTERCEPT_DR7_WRITE),
		.exit_code = SVM_EXIT_WRITE_DR7,
		.exit_info_1 = 3,
		.exit_info_1_mask = ~0ULL,
	}, {
		.name = "MOV-from-DR7",
		.code = l2_mov_from_dr7_code,
		.fep_code = l2_fep_mov_from_dr7_code,
		.intercept_dr = BIT(INTERCEPT_DR7_READ),
		.exit_code = SVM_EXIT_READ_DR7,
		.exit_info_1 = 10,
		.exit_info_1_mask = ~0ULL,
	}, {
		.name = "CLTS",
		.code = l2_clts_code,
		.fep_code = l2_fep_clts_code,
		.intercept_cr = BIT(INTERCEPT_CR0_WRITE),
		.exit_code = SVM_EXIT_WRITE_CR0,
		.exit_info_1_mask = BIT_ULL(63),
	}, {
		.name = "LMSW",
		.code = l2_lmsw_code,
		.fep_code = l2_fep_lmsw_code,
		.intercept_cr = BIT(INTERCEPT_CR0_WRITE),
		.exit_code = SVM_EXIT_WRITE_CR0,
		.exit_info_1_mask = BIT_ULL(63),
	}, {
		.name = "SMSW",
		.code = l2_smsw_code,
		.fep_code = l2_fep_smsw_code,
		.intercept_cr = BIT(INTERCEPT_CR0_READ),
		.exit_code = SVM_EXIT_READ_CR0,
		.exit_info_1_mask = BIT_ULL(63),
	}, {
		.name = "INTn",
		.code = l2_int_code,
		.fep_code = l2_fep_int_code,
		.intercept = BIT_ULL(INTERCEPT_INTn),
		.exit_code = SVM_EXIT_SWINT,
		.exit_info_1 = TEST_INT_VECTOR,
		.exit_info_1_mask = ~0ULL,
	}, {
		.name = "INVLPG",
		.code = l2_invlpg_code,
		.fep_code = l2_fep_invlpg_code,
		.intercept = BIT_ULL(INTERCEPT_INVLPG),
		.exit_code = SVM_EXIT_INVLPG,
		.exit_info_1 = (u64)&npf_target,
		.exit_info_1_mask = ~0ULL,
	}, {
		.name = "INVLPGA",
		.code = l2_invlpga_code,
		.fep_code = l2_fep_invlpga_code,
		.intercept = BIT_ULL(INTERCEPT_INVLPGA),
		.exit_code = SVM_EXIT_INVLPGA,
		.exit_info_1_mask = ~0ULL,
		.check_rax = true,
		.rax = (u64)&npf_target,
	},
};

static void assert_decode_assist_insn_bytes(struct vmcb *vmcb)
{
	GUEST_ASSERT(vmcb->control.insn_len);
	GUEST_ASSERT(vmcb->control.insn_len <=
		     sizeof(vmcb->control.insn_bytes));
	GUEST_ASSERT(!memcmp(vmcb->control.insn_bytes,
			     (void *)vmcb->save.rip,
			     vmcb->control.insn_len));
}

static void assert_full_decode_assist_insn_bytes(struct vmcb *vmcb)
{
	GUEST_ASSERT_EQ(vmcb->control.insn_len,
			sizeof(vmcb->control.insn_bytes));
	assert_decode_assist_insn_bytes(vmcb);
}

static void prepare_l2_for_vmrun(struct vmcb *vmcb, gva_t rip)
{
	vmcb->save.rip = rip;
	vmcb->save.rsp = (u64)&l2_guest_stack[L2_GUEST_STACK_SIZE];
}

static void run_intercept_test(struct svm_test_data *svm,
			       const struct instruction_intercept_test *test,
			       bool synthesized)
{
	struct vmcb *vmcb = svm->vmcb;
	struct vmcb_control_area *control = &vmcb->control;
	const char *source = synthesized ? "synthesized" : "hardware";
	u64 expected_exit_info_1 = test->exit_info_1 & test->exit_info_1_mask;

	control->intercept |= test->intercept;
	control->intercept_cr |= test->intercept_cr;
	control->intercept_dr |= test->intercept_dr;

	if (synthesized) {
		control->exit_info_1 = ~0ULL;
		control->exit_info_2 = ~0ULL;
		prepare_l2_for_vmrun(vmcb, (u64)test->fep_code);
	} else {
		prepare_l2_for_vmrun(vmcb, (u64)test->code);
	}

	run_guest(vmcb, svm->vmcb_gpa);

	__GUEST_ASSERT(control->exit_code == test->exit_code,
		       "%s (%s): expected exit code %#lx, got %#lx",
		       test->name, source, (unsigned long)test->exit_code,
		       (unsigned long)control->exit_code);
	__GUEST_ASSERT((control->exit_info_1 & test->exit_info_1_mask) ==
		       expected_exit_info_1,
		       "%s (%s): expected EXITINFO1 %#lx with mask %#lx, got %#lx",
		       test->name, source, (unsigned long)expected_exit_info_1,
		       (unsigned long)test->exit_info_1_mask,
		       (unsigned long)control->exit_info_1);
	__GUEST_ASSERT(!control->insn_len,
		       "%s (%s): expected no instruction bytes, got %u",
		       test->name, source, control->insn_len);

	if (test->check_rax)
		__GUEST_ASSERT(vmcb->save.rax == test->rax,
			       "%s (%s): expected rAX %#lx, got %#lx",
			       test->name, source, (unsigned long)test->rax,
			       (unsigned long)vmcb->save.rax);

	if (synthesized)
		__GUEST_ASSERT(!control->exit_info_2,
			       "%s (%s): expected EXITINFO2 to be clear, got %#lx",
			       test->name, source,
			       (unsigned long)control->exit_info_2);

	control->intercept &= ~test->intercept;
	control->intercept_cr &= ~test->intercept_cr;
	control->intercept_dr &= ~test->intercept_dr;
}

static void test_instruction_intercepts(struct svm_test_data *svm)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(instruction_intercept_tests); i++) {
		run_intercept_test(svm, &instruction_intercept_tests[i], false);

		if (is_forced_emulation_enabled)
			run_intercept_test(svm, &instruction_intercept_tests[i],
					   true);
	}
}

/*
 * A hardware #NPF (data read of a GPA not mapped in the NPT) must reflect
 * the instruction bytes recorded by hardware.
 */
static void test_hardware_npf(struct svm_test_data *svm, gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb, (u64)l2_read_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	assert_decode_assist_insn_bytes(vmcb);
}

/* An instruction-fetch #NPF must not report DecodeAssist instruction bytes. */
static void test_hardware_fetch_npf(struct svm_test_data *svm,
				    gva_t fetch_npf_gva,
				    gpa_t fetch_npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb, fetch_npf_gva);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, fetch_npf_gpa);
	GUEST_ASSERT(vmcb->control.exit_info_1 & PFERR_FETCH_MASK);
	GUEST_ASSERT_EQ(vmcb->control.insn_len, 0);
}

/*
 * The IOIO intercept causes L0 to emulate OUTSB before accessing its source
 * operand.  The emulated read then faults on L1's NPT, resulting in a
 * KVM-synthesized #NPF.
 */
static void test_synthesized_npf(struct svm_test_data *svm, gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb, (u64)l2_outsb_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	assert_full_decode_assist_insn_bytes(vmcb);
}

/*
 * OUTSB is the final byte of a mapped code page, and the following page is
 * not present in L2's page tables.  DecodeAssist byte fetching must stop at
 * the page boundary and report only the OUTSB opcode.
 */
static void test_synthesized_npf_truncated(struct svm_test_data *svm,
					   gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb,
			     (u64)&boundary_outsb_code[PAGE_SIZE -
						       BOUNDARY_OUTSB_CODE_SIZE]);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	GUEST_ASSERT_EQ(vmcb->save.rip,
			(u64)&boundary_outsb_code[PAGE_SIZE - 1]);
	GUEST_ASSERT_EQ(vmcb->control.insn_len, 1);
	GUEST_ASSERT_EQ(vmcb->control.insn_bytes[0], OUTSB_OPCODE);
}

/* A non-64-bit code segment must truncate the byte window at CS.limit. */
static void test_synthesized_npf_cs_limit(struct svm_test_data *svm,
					  gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;
	u16 cs_attrib = vmcb->save.cs.attrib;
	u32 cs_limit = vmcb->save.cs.limit;

	vmcb->save.cs.attrib &= ~SVM_SELECTOR_L_MASK;
	vmcb->save.cs.attrib |= SVM_SELECTOR_DB_MASK;
	vmcb->save.cs.limit =
		(u32)(u64)&limit_outsb_code[LIMIT_OUTSB_OPCODE_OFFSET];
	prepare_l2_for_vmrun(vmcb, (u64)limit_outsb_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	GUEST_ASSERT_EQ(vmcb->save.rip,
			(u64)&limit_outsb_code[LIMIT_OUTSB_OPCODE_OFFSET]);
	GUEST_ASSERT_EQ(vmcb->control.insn_len, 1);
	GUEST_ASSERT_EQ(vmcb->control.insn_bytes[0], OUTSB_OPCODE);
	vmcb->save.cs.attrib = cs_attrib;
	vmcb->save.cs.limit = cs_limit;
}

static void test_intercepted_pf(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb, (u64)l2_pf_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_EXCP_BASE + PF_VECTOR);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, PF_TEST_GVA);
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_PRESENT_MASK));
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_FETCH_MASK));
	assert_decode_assist_insn_bytes(vmcb);
}

/* An intercepted instruction-fetch #PF likewise reports no instruction bytes. */
static void test_intercepted_fetch_pf(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb, PF_FETCH_TEST_GVA);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_EXCP_BASE + PF_VECTOR);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, PF_FETCH_TEST_GVA);
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_PRESENT_MASK));
	GUEST_ASSERT(vmcb->control.exit_info_1 & PFERR_FETCH_MASK);
	GUEST_ASSERT_EQ(vmcb->control.insn_len, 0);
}

static void test_synthesized_pf(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	if (!is_forced_emulation_enabled)
		return;

	prepare_l2_for_vmrun(vmcb, (u64)l2_fep_pf_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_EXCP_BASE + PF_VECTOR);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, PF_TEST_GVA);
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_PRESENT_MASK));
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_FETCH_MASK));
	assert_full_decode_assist_insn_bytes(vmcb);
}

/* Userspace injects this #PF without a matching emulator fetch cache. */
static void test_userspace_injected_pf(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb, (u64)l2_userspace_pf_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_EXCP_BASE + PF_VECTOR);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, PF_TEST_GVA);
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_FETCH_MASK));
	assert_full_decode_assist_insn_bytes(vmcb);
}

static void test_vmmcall(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(vmcb, (u64)l2_vmmcall_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT_EQ(vmcb->control.insn_len, 0);
}

static void l1_guest_code(struct svm_test_data *svm, gpa_t npf_gpa,
			  gva_t fetch_npf_gva,
			  gpa_t fetch_npf_gpa, gpa_t iopm_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	GUEST_ASSERT(this_cpu_has(X86_FEATURE_DECODEASSISTS));

	generic_svm_setup(svm, l2_read_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);
	vmcb->control.iopm_base_pa = iopm_gpa;

	vmcb->control.intercept |= BIT_ULL(INTERCEPT_IOIO_PROT);
	vmcb->control.intercept_exceptions |= 1U << PF_VECTOR;

	test_hardware_npf(svm, npf_gpa);
	test_hardware_fetch_npf(svm, fetch_npf_gva, fetch_npf_gpa);
	test_synthesized_npf(svm, npf_gpa);
	test_synthesized_npf_truncated(svm, npf_gpa);
	test_synthesized_npf_cs_limit(svm, npf_gpa);
	test_intercepted_pf(svm);
	test_intercepted_fetch_pf(svm);
	test_synthesized_pf(svm);
	test_userspace_injected_pf(svm);
	test_instruction_intercepts(svm);
	test_vmmcall(svm);

	GUEST_DONE();
}

static void prepare_boundary_outsb_code(struct kvm_vm *vm)
{
	gva_t code_gva = (gva_t)&boundary_outsb_code[PAGE_SIZE -
						      BOUNDARY_OUTSB_CODE_SIZE];
	u8 *code = addr_gva2hva(vm, code_gva);
	u64 source = (u64)&npf_target;

	/* movabs $npf_target, %rsi */
	code[0] = 0x48;
	code[1] = 0xbe;
	memcpy(&code[2], &source, sizeof(source));

	/* mov $0x80, %dx; outsb */
	code[10] = 0x66;
	code[11] = 0xba;
	code[12] = 0x80;
	code[13] = 0x00;
	code[14] = OUTSB_OPCODE;
}

static void prepare_limit_outsb_code(struct kvm_vm *vm)
{
	u8 *code = addr_gva2hva(vm, (gva_t)limit_outsb_code);
	u32 source = (u32)(u64)&npf_target;

	TEST_ASSERT((u64)&npf_target <= UINT32_MAX,
		    "npf_target must be addressable from compatibility mode");
	TEST_ASSERT((u64)&limit_outsb_code[LIMIT_OUTSB_OPCODE_OFFSET] <=
		    UINT32_MAX,
		    "limit_outsb_code must be addressable from compatibility mode");

	/* mov $npf_target, %esi; mov $0x80, %dx; outsb */
	code[0] = 0xbe;
	memcpy(&code[1], &source, sizeof(source));
	code[5] = 0x66;
	code[6] = 0xba;
	code[7] = 0x80;
	code[8] = 0x00;
	code[LIMIT_OUTSB_OPCODE_OFFSET] = OUTSB_OPCODE;
}

static void queue_userspace_pf(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_events events;

	vcpu_events_get(vcpu, &events);
	TEST_ASSERT(!events.exception.pending && !events.exception.injected,
		    "Unexpected exception queued before userspace #PF injection");
	TEST_ASSERT(events.flags & KVM_VCPUEVENT_VALID_PAYLOAD,
		    "KVM_CAP_EXCEPTION_PAYLOAD was not enabled");

	events.flags |= KVM_VCPUEVENT_VALID_PAYLOAD;
	events.exception.injected = false;
	events.exception.pending = true;
	events.exception.nr = PF_VECTOR;
	events.exception.has_error_code = true;
	events.exception.error_code = 0;
	events.exception_has_payload = true;
	events.exception_payload = PF_TEST_GVA;
	vcpu_events_set(vcpu, &events);
}

int main(int argc, char *argv[])
{
	gva_t svm_gva, npf_gva, fetch_npf_gva, boundary_page_gva, iopm_gva;
	gpa_t npf_gpa, fetch_npf_gpa, iopm_gpa;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	u64 *pte;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_NPT));
	TEST_REQUIRE(this_cpu_has(X86_FEATURE_DECODEASSISTS));
	TEST_ASSERT(kvm_cpu_has(X86_FEATURE_DECODEASSISTS),
		    "KVM failed to expose DecodeAssists");
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_EXCEPTION_PAYLOAD));

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
	vm_enable_cap(vm, KVM_CAP_EXCEPTION_PAYLOAD, 1);
	prepare_boundary_outsb_code(vm);
	prepare_limit_outsb_code(vm);
	vm_enable_npt(vm);
	vcpu_alloc_svm(vm, &svm_gva);
	iopm_gva = vm_alloc_pages(vm, TEST_IOPM_SIZE / PAGE_SIZE);
	iopm_gpa = addr_gva2gpa(vm, iopm_gva);
	memset(addr_gva2hva(vm, iopm_gva), 0, TEST_IOPM_SIZE);
	npf_gva = (gva_t)&npf_target;
	npf_gpa = addr_gva2gpa(vm, npf_gva);
	fetch_npf_gva = vm_alloc_page(vm);
	fetch_npf_gpa = addr_gva2gpa(vm, fetch_npf_gva);
	*(u8 *)addr_gva2hva(vm, fetch_npf_gva) = INT3_OPCODE;

	tdp_identity_map_default_memslots(vm);
	pte = tdp_get_pte(vm, npf_gpa);
	*pte &= ~PTE_PRESENT_MASK(&vm->stage2_mmu);
	pte = tdp_get_pte(vm, fetch_npf_gpa);
	*pte &= ~PTE_PRESENT_MASK(&vm->stage2_mmu);

	boundary_page_gva = (gva_t)&boundary_outsb_code[PAGE_SIZE];
	pte = vm_get_pte(vm, boundary_page_gva);
	*pte &= ~PTE_PRESENT_MASK(&vm->mmu);

	vcpu_args_set(vcpu, 5, svm_gva, npf_gpa, fetch_npf_gva,
		      fetch_npf_gpa, iopm_gpa);
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	{
		struct ucall uc;

		TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
		TEST_ASSERT_EQ(uc.args[1], USERSPACE_PF_STAGE);
	}
	queue_userspace_pf(vcpu);
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, NULL), UCALL_DONE);

	kvm_vm_free(vm);
	return 0;
}
