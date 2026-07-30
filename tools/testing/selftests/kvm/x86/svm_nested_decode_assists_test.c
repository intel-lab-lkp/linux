// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test KVM's virtualization of SVM DecodeAssists for nested guests.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"

#define TEST_INT_VECTOR 0x81

/* Any canonical virtual address that is never mapped by the selftest VM. */
#define PF_TEST_GVA BIT_ULL(40)
#define PF_FETCH_TEST_GVA BIT_ULL(41)

#define OUTSB_OPCODE 0x6e
#define BOUNDARY_OUTSB_CODE_SIZE 15
#define LINEAR_WRAP_CODE_GVA (BIT_ULL(32) - PAGE_SIZE)
#define LINEAR_WRAP_OUTSB_OFFSET (PAGE_SIZE - 8)
#define LINEAR_WRAP_SETUP_SIZE 9
#define LINEAR_WRAP_SETUP_OFFSET \
	(LINEAR_WRAP_OUTSB_OFFSET - LINEAR_WRAP_SETUP_SIZE)
#define LINEAR_WRAP_CS_BASE PAGE_SIZE
#define TEST_IOPM_SIZE (3 * PAGE_SIZE)

static u8 npf_target[PAGE_SIZE] __aligned(PAGE_SIZE);
static u8 mmio_source __aligned(PAGE_SIZE);
static u8 boundary_outsb_code[2 * PAGE_SIZE] __aligned(PAGE_SIZE);
static const u8 linear_wrap_insn_bytes[15] = {
	OUTSB_OPCODE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

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

static void l2_movsb_code(void)
{
	asm volatile("mov %0, %%rsi\n\t"
		     "mov %1, %%rdi\n\t"
		     "movsb"
		     : : "r"(&mmio_source), "r"(&npf_target)
		     : "rsi", "rdi", "memory");
	GUEST_FAIL("L2 MOVSB did not cause a nested page fault");
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

static void l2_stale_emulator_pf_code(void)
{
	asm volatile("movb (%0), %%al\n\t"
		     "nop"
		     : : "r"(&mmio_source) : "rax", "memory");
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

static void prepare_l2_for_vmrun(struct svm_test_data *svm, gva_t rip)
{
	struct vmcb *vmcb = svm->vmcb;

	vmcb->save.rip = rip;
	vmcb->save.rsp = (u64)svm->stack;
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
		prepare_l2_for_vmrun(svm, (u64)test->fep_code);
	} else {
		prepare_l2_for_vmrun(svm, (u64)test->code);
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

static void test_hardware_npf(struct svm_test_data *svm, gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(svm, (u64)l2_read_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	assert_decode_assist_insn_bytes(vmcb);
}

/*
 * The IOIO intercept causes L0 to emulate OUTSB before accessing its source
 * operand.  The emulated read then faults on L1's NPT, resulting in a
 * KVM-synthesized #NPF.
 */
static void test_synthesized_npf(struct svm_test_data *svm, gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(svm, (u64)l2_outsb_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	assert_full_decode_assist_insn_bytes(vmcb);
}

/*
 * MOVSB first reads from MMIO, causing a hardware #NPF that L0 emulates.
 * After userspace completes the read, the emulated destination write faults
 * on L1's NPT.  The new #NPF must not reuse the original hardware exit's GPA.
 */
static void test_synthesized_npf_after_hardware_npf(struct svm_test_data *svm,
						    gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(svm, (u64)l2_movsb_code);
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

	prepare_l2_for_vmrun(svm,
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

/*
 * OUTSB is the final byte in the lower canonical address range.  The first
 * byte after OUTSB is non-canonical, and must terminate DecodeAssist fetching
 * even though the corresponding high canonical address is mapped.
 */
static void test_synthesized_npf_canonical_boundary(struct svm_test_data *svm,
						    gpa_t npf_gpa,
						    gva_t code_gva)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(svm, code_gva + PAGE_SIZE -
				  BOUNDARY_OUTSB_CODE_SIZE);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	GUEST_ASSERT_EQ(vmcb->save.rip, code_gva + PAGE_SIZE - 1);
	GUEST_ASSERT_EQ(vmcb->control.insn_len, 1);
	GUEST_ASSERT_EQ(vmcb->control.insn_bytes[0], OUTSB_OPCODE);
}

/*
 * A nonzero CS.base makes the DecodeAssist byte window cross the 32-bit
 * linear-address boundary without crossing the CS limit.
 */
static void test_synthesized_npf_linear_wrap(struct svm_test_data *svm,
					     gpa_t npf_gpa)
{
	struct vmcb *vmcb = svm->vmcb;
	u16 cs_attrib = vmcb->save.cs.attrib;
	u64 cs_base = vmcb->save.cs.base;
	u32 cs_limit = vmcb->save.cs.limit;
	u32 outsb_eip = LINEAR_WRAP_CODE_GVA + LINEAR_WRAP_OUTSB_OFFSET -
			LINEAR_WRAP_CS_BASE;
	u32 setup_eip = LINEAR_WRAP_CODE_GVA + LINEAR_WRAP_SETUP_OFFSET -
			LINEAR_WRAP_CS_BASE;

	vmcb->save.cs.attrib &= ~SVM_SELECTOR_L_MASK;
	vmcb->save.cs.attrib |= SVM_SELECTOR_DB_MASK;
	vmcb->save.cs.base = LINEAR_WRAP_CS_BASE;
	vmcb->save.cs.limit = UINT32_MAX;
	prepare_l2_for_vmrun(svm, setup_eip);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_NPF);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, npf_gpa);
	GUEST_ASSERT_EQ(vmcb->save.rip, outsb_eip);
	GUEST_ASSERT_EQ(vmcb->control.insn_len,
			sizeof(linear_wrap_insn_bytes));
	GUEST_ASSERT(!memcmp(vmcb->control.insn_bytes,
			     linear_wrap_insn_bytes,
			     sizeof(linear_wrap_insn_bytes)));
	vmcb->save.cs.attrib = cs_attrib;
	vmcb->save.cs.base = cs_base;
	vmcb->save.cs.limit = cs_limit;
}

static void test_hardware_intercepted_pf(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(svm, (u64)l2_pf_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_EXCP_BASE + PF_VECTOR);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, PF_TEST_GVA);
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_PRESENT_MASK));
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_FETCH_MASK));
	assert_decode_assist_insn_bytes(vmcb);
}

static void test_hardware_intercepted_fetch_pf(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(svm, PF_FETCH_TEST_GVA);
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

	prepare_l2_for_vmrun(svm, (u64)l2_fep_pf_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_EXCP_BASE + PF_VECTOR);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, PF_TEST_GVA);
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_PRESENT_MASK));
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_FETCH_MASK));
	assert_full_decode_assist_insn_bytes(vmcb);
}

/*
 * Inject #PF while an emulated MMIO read is awaiting completion.  KVM
 * completes the instruction before constructing the nested VM-Exit, so the
 * old emulator fetch cache must not be reported for the following instruction.
 */
static void test_userspace_injected_pf_during_emulation(struct svm_test_data *svm)
{
	struct vmcb *vmcb = svm->vmcb;

	prepare_l2_for_vmrun(svm, (u64)l2_stale_emulator_pf_code);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_EXCP_BASE + PF_VECTOR);
	GUEST_ASSERT_EQ(vmcb->control.exit_info_2, PF_TEST_GVA);
	GUEST_ASSERT(!(vmcb->control.exit_info_1 & PFERR_FETCH_MASK));
	assert_full_decode_assist_insn_bytes(vmcb);
}

static void l1_guest_code(struct svm_test_data *svm, gpa_t npf_gpa,
			  gpa_t iopm_gpa, gva_t canonical_code_gva)
{
	struct vmcb *vmcb = svm->vmcb;

	GUEST_ASSERT(this_cpu_has(X86_FEATURE_DECODEASSISTS));

	generic_svm_setup(svm, l2_read_code);
	vmcb->control.iopm_base_pa = iopm_gpa;

	vmcb->control.intercept |= BIT_ULL(INTERCEPT_IOIO_PROT);
	vmcb->control.intercept_exceptions |= 1U << PF_VECTOR;

	test_hardware_npf(svm, npf_gpa);
	test_synthesized_npf(svm, npf_gpa);
	test_synthesized_npf_after_hardware_npf(svm, npf_gpa);
	test_synthesized_npf_truncated(svm, npf_gpa);
	test_synthesized_npf_canonical_boundary(svm, npf_gpa,
						canonical_code_gva);
	test_synthesized_npf_linear_wrap(svm, npf_gpa);
	test_hardware_intercepted_pf(svm);
	test_hardware_intercepted_fetch_pf(svm);
	test_synthesized_pf(svm);
	test_userspace_injected_pf_during_emulation(svm);
	test_instruction_intercepts(svm);

	GUEST_DONE();
}

static void build_boundary_outsb_code(u8 *code)
{
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

static void prepare_boundary_outsb_code(struct kvm_vm *vm)
{
	gva_t code_gva = (gva_t)&boundary_outsb_code[PAGE_SIZE -
						      BOUNDARY_OUTSB_CODE_SIZE];

	build_boundary_outsb_code(addr_gva2hva(vm, code_gva));
}

static gva_t prepare_canonical_boundary_outsb_code(struct kvm_vm *vm)
{
	gva_t backing_gva = vm_alloc_pages(vm, 2);
	u8 *code_page = addr_gva2hva(vm, backing_gva);
	u8 *alias_page = addr_gva2hva(vm, backing_gva + PAGE_SIZE);
	gpa_t code_gpa = addr_gva2gpa(vm, backing_gva);
	gpa_t alias_gpa = addr_gva2gpa(vm, backing_gva + PAGE_SIZE);
	gva_t code_gva = BIT_ULL(vm->va_bits - 1) - PAGE_SIZE;
	gva_t alias_gva = ~(BIT_ULL(vm->va_bits - 1) - 1);
	u8 *code = &code_page[PAGE_SIZE - BOUNDARY_OUTSB_CODE_SIZE];

	build_boundary_outsb_code(code);
	memset(alias_page, 0xcc, PAGE_SIZE);
	virt_map(vm, code_gva, code_gpa, 1);
	virt_map(vm, alias_gva, alias_gpa, 1);

	return code_gva;
}

static void prepare_linear_wrap_outsb_code(struct kvm_vm *vm)
{
	gva_t code_gva = vm_alloc_pages(vm, 2);
	u8 *high_page = addr_gva2hva(vm, code_gva);
	u8 *low_page = addr_gva2hva(vm, code_gva + PAGE_SIZE);
	gpa_t high_gpa = addr_gva2gpa(vm, code_gva);
	gpa_t low_gpa = addr_gva2gpa(vm, code_gva + PAGE_SIZE);
	u32 source = (u32)(u64)&npf_target;
	u8 *setup = &high_page[LINEAR_WRAP_SETUP_OFFSET];

	TEST_ASSERT((u64)&npf_target <= UINT32_MAX,
		    "npf_target must be addressable from compatibility mode");

	/* mov $npf_target, %esi; mov $0x80, %dx */
	setup[0] = 0xbe;
	memcpy(&setup[1], &source, sizeof(source));
	setup[5] = 0x66;
	setup[6] = 0xba;
	setup[7] = 0x80;
	setup[8] = 0x00;

	memcpy(&high_page[LINEAR_WRAP_OUTSB_OFFSET],
	       linear_wrap_insn_bytes,
	       PAGE_SIZE - LINEAR_WRAP_OUTSB_OFFSET);
	memcpy(low_page,
	       &linear_wrap_insn_bytes[PAGE_SIZE - LINEAR_WRAP_OUTSB_OFFSET],
	       sizeof(linear_wrap_insn_bytes) -
	       (PAGE_SIZE - LINEAR_WRAP_OUTSB_OFFSET));

	virt_map(vm, LINEAR_WRAP_CODE_GVA, high_gpa, 1);
	virt_map(vm, 0, low_gpa, 1);
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

static void complete_mmio_read(struct kvm_vcpu *vcpu, gpa_t expected_gpa,
			       u8 value)
{
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_MMIO);
	TEST_ASSERT(!vcpu->run->mmio.is_write,
		    "Expected an MMIO read, got a write");
	TEST_ASSERT_EQ(vcpu->run->mmio.phys_addr, expected_gpa);
	TEST_ASSERT_EQ(vcpu->run->mmio.len, 1);
	vcpu->run->mmio.data[0] = value;
}

static void assert_ucall_done(struct kvm_vcpu *vcpu)
{
	struct ucall uc;
	u64 actual;

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	actual = get_ucall(vcpu, &uc);
	if (actual == UCALL_ABORT)
		REPORT_GUEST_ASSERT(uc);

	TEST_ASSERT_EQ(actual, UCALL_DONE);
}

int main(int argc, char *argv[])
{
	gva_t svm_gva, npf_gva, boundary_page_gva, iopm_gva;
	gva_t canonical_code_gva;
	gpa_t npf_gpa, mmio_source_gpa, mmio_gpa, iopm_gpa;
	struct userspace_mem_region *region;
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
	canonical_code_gva = prepare_canonical_boundary_outsb_code(vm);
	prepare_linear_wrap_outsb_code(vm);
	vm_enable_npt(vm);
	vcpu_alloc_svm(vm, &svm_gva);
	iopm_gva = vm_alloc_pages(vm, TEST_IOPM_SIZE / PAGE_SIZE);
	iopm_gpa = addr_gva2gpa(vm, iopm_gva);
	memset(addr_gva2hva(vm, iopm_gva), 0, TEST_IOPM_SIZE);
	npf_gva = (gva_t)&npf_target;
	npf_gpa = addr_gva2gpa(vm, npf_gva);

	tdp_identity_map_default_memslots(vm);
	pte = tdp_get_pte(vm, npf_gpa);
	*pte &= ~PTE_PRESENT_MASK(&vm->stage2_mmu);
	region = memslot2region(vm, 0);
	mmio_gpa = region->region.guest_phys_addr +
		   region->region.memory_size + PAGE_SIZE;
	mmio_source_gpa = addr_gva2gpa(vm, (gva_t)&mmio_source);
	pte = tdp_get_pte(vm, mmio_source_gpa);
	*pte = (*pte & ~PHYSICAL_PAGE_MASK) | mmio_gpa;

	boundary_page_gva = (gva_t)&boundary_outsb_code[PAGE_SIZE];
	pte = vm_get_pte(vm, boundary_page_gva);
	*pte &= ~PTE_PRESENT_MASK(&vm->mmu);

	vcpu_args_set(vcpu, 4, svm_gva, npf_gpa, iopm_gpa,
		      canonical_code_gva);

	/* Complete the MMIO source read in the MOVSB #NPF regression test. */
	vcpu_run(vcpu);
	complete_mmio_read(vcpu, mmio_gpa, 0xa5);

	/* Leave the second MMIO read pending while injecting #PF. */
	vcpu_run(vcpu);
	complete_mmio_read(vcpu, mmio_gpa, 0x5a);
	queue_userspace_pf(vcpu);
	vcpu_run(vcpu);
	assert_ucall_done(vcpu);

	kvm_vm_free(vm);
	return 0;
}
