// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM nested SVM PAT test
 *
 * Copyright (C) 2026, Google LLC.
 *
 * Test that KVM correctly virtualizes the PAT MSR and VMCB g_pat field
 * for nested SVM guests:
 *
 * o With nested NPT disabled:
 *     - L1 and L2 share the same PAT
 *     - The vmcb12.g_pat is ignored
 * o With nested NPT enabled:
 *     - Invalid g_pat in vmcb12 should cause VMEXIT_INVALID
 *     - L2 should see vmcb12.g_pat via RDMSR, not L1's PAT
 *     - L2's writes to PAT should be saved to vmcb12 on exit
 *     - L1's PAT should be restored after #VMEXIT from L2
 *     - State save/restore should preserve both L1's and L2's PAT values
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"

#define L2_GUEST_STACK_SIZE 256

#define PAT_DEFAULT		0x0007040600070406ULL
#define L1_PAT_VALUE		0x0007040600070404ULL  /* Change PA0 to WT */
#define L2_VMCB12_PAT		0x0606060606060606ULL  /* All WB */
#define L2_PAT_MODIFIED		0x0606060606060604ULL  /* Change PA0 to WT */
#define INVALID_PAT_VALUE	0x0808080808080808ULL  /* 8 is reserved */

/*
 * Shared state between L1 and L2 for verification.
 */
struct pat_test_data {
	uint64_t l2_pat_read;
	uint64_t l2_pat_after_write;
	uint64_t l1_pat_after_vmexit;
	uint64_t vmcb12_gpat_after_exit;
	bool l2_done;
};

static struct pat_test_data *pat_data;

static void l2_guest_code(void)
{
	pat_data->l2_pat_read = rdmsr(MSR_IA32_CR_PAT);
	wrmsr(MSR_IA32_CR_PAT, L2_PAT_MODIFIED);
	pat_data->l2_pat_after_write = rdmsr(MSR_IA32_CR_PAT);
	pat_data->l2_done = true;
	vmmcall();
}

static void l2_guest_code_saverestoretest(void)
{
	pat_data->l2_pat_read = rdmsr(MSR_IA32_CR_PAT);

	GUEST_SYNC(1);
	GUEST_ASSERT_EQ(rdmsr(MSR_IA32_CR_PAT), pat_data->l2_pat_read);

	wrmsr(MSR_IA32_CR_PAT, L2_PAT_MODIFIED);
	pat_data->l2_pat_after_write = rdmsr(MSR_IA32_CR_PAT);

	GUEST_SYNC(2);
	GUEST_ASSERT_EQ(rdmsr(MSR_IA32_CR_PAT), L2_PAT_MODIFIED);

	pat_data->l2_done = true;
	vmmcall();
}

static void l2_guest_code_multi_vmentry(void)
{
	pat_data->l2_pat_read = rdmsr(MSR_IA32_CR_PAT);
	wrmsr(MSR_IA32_CR_PAT, L2_PAT_MODIFIED);
	pat_data->l2_pat_after_write = rdmsr(MSR_IA32_CR_PAT);
	vmmcall();

	pat_data->l2_pat_read = rdmsr(MSR_IA32_CR_PAT);
	pat_data->l2_done = true;
	vmmcall();
}

static struct vmcb *l1_common_setup(struct svm_test_data *svm,
				    struct pat_test_data *data,
				    void *l2_guest_code,
				    void *l2_guest_stack)
{
	struct vmcb *vmcb = svm->vmcb;

	pat_data = data;

	wrmsr(MSR_IA32_CR_PAT, L1_PAT_VALUE);
	GUEST_ASSERT_EQ(rdmsr(MSR_IA32_CR_PAT), L1_PAT_VALUE);

	generic_svm_setup(svm, l2_guest_code, l2_guest_stack);

	vmcb->save.g_pat = L2_VMCB12_PAT;
	vmcb->control.intercept &= ~(1ULL << INTERCEPT_MSR_PROT);

	return vmcb;
}

static void l1_assert_l2_state(struct pat_test_data *data, uint64_t expected_pat_read)
{
	GUEST_ASSERT(data->l2_done);
	GUEST_ASSERT_EQ(data->l2_pat_read, expected_pat_read);
	GUEST_ASSERT_EQ(data->l2_pat_after_write, L2_PAT_MODIFIED);
}

static void l1_svm_code_npt_disabled(struct svm_test_data *svm,
				     struct pat_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb;

	vmcb = l1_common_setup(svm, data, l2_guest_code,
			       &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	l1_assert_l2_state(data, L1_PAT_VALUE);

	data->l1_pat_after_vmexit = rdmsr(MSR_IA32_CR_PAT);
	GUEST_ASSERT_EQ(data->l1_pat_after_vmexit, L2_PAT_MODIFIED);

	GUEST_DONE();
}

static void l1_svm_code_invalid_gpat(struct svm_test_data *svm,
				     struct pat_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb;

	vmcb = l1_common_setup(svm, data, l2_guest_code,
			       &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	vmcb->save.g_pat = INVALID_PAT_VALUE;

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_ERR);
	GUEST_ASSERT(!data->l2_done);

	GUEST_DONE();
}

static void l1_svm_code_npt_enabled(struct svm_test_data *svm,
				    struct pat_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb;

	vmcb = l1_common_setup(svm, data, l2_guest_code,
			       &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	l1_assert_l2_state(data, L2_VMCB12_PAT);

	data->vmcb12_gpat_after_exit = vmcb->save.g_pat;
	GUEST_ASSERT_EQ(data->vmcb12_gpat_after_exit, L2_PAT_MODIFIED);

	data->l1_pat_after_vmexit = rdmsr(MSR_IA32_CR_PAT);
	GUEST_ASSERT_EQ(data->l1_pat_after_vmexit, L1_PAT_VALUE);

	GUEST_DONE();
}

static void l1_svm_code_saverestore(struct svm_test_data *svm,
				    struct pat_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb;

	vmcb = l1_common_setup(svm, data, l2_guest_code_saverestoretest,
			       &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT(data->l2_done);

	GUEST_ASSERT_EQ(rdmsr(MSR_IA32_CR_PAT), L1_PAT_VALUE);
	GUEST_ASSERT_EQ(vmcb->save.g_pat, L2_PAT_MODIFIED);

	GUEST_DONE();
}

static void l1_svm_code_multi_vmentry(struct svm_test_data *svm,
				      struct pat_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb;

	vmcb = l1_common_setup(svm, data, l2_guest_code_multi_vmentry,
			       &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);

	GUEST_ASSERT_EQ(data->l2_pat_after_write, L2_PAT_MODIFIED);
	GUEST_ASSERT_EQ(vmcb->save.g_pat, L2_PAT_MODIFIED);
	GUEST_ASSERT_EQ(rdmsr(MSR_IA32_CR_PAT), L1_PAT_VALUE);

	vmcb->save.rip += 3;  /* vmmcall */
	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT(data->l2_done);
	GUEST_ASSERT_EQ(data->l2_pat_read, L2_PAT_MODIFIED);
	GUEST_ASSERT_EQ(rdmsr(MSR_IA32_CR_PAT), L1_PAT_VALUE);

	GUEST_DONE();
}

static void run_test(void *l1_code, const char *test_name, bool npt_enabled,
		     bool do_save_restore)
{
	struct pat_test_data *data_hva;
	vm_vaddr_t svm_gva, data_gva;
	struct kvm_x86_state *state;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	pr_info("Testing: %s\n", test_name);

	vm = vm_create_with_one_vcpu(&vcpu, l1_code);
	if (npt_enabled)
		vm_enable_npt(vm);

	vcpu_alloc_svm(vm, &svm_gva);

	data_gva = vm_vaddr_alloc_page(vm);
	data_hva = addr_gva2hva(vm, data_gva);
	memset(data_hva, 0, sizeof(*data_hva));

	if (npt_enabled)
		tdp_identity_map_default_memslots(vm);

	vcpu_args_set(vcpu, 2, svm_gva, data_gva);

	for (;;) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_SYNC:
			if (do_save_restore) {
				pr_info("  Save/restore at sync point %ld\n",
					uc.args[1]);
				state = vcpu_save_state(vcpu);
				kvm_vm_release(vm);
				vcpu = vm_recreate_with_one_vcpu(vm);
				vcpu_load_state(vcpu, state);
				kvm_x86_state_cleanup(state);
			}
			break;
		case UCALL_DONE:
			pr_info("  PASSED\n");
			kvm_vm_free(vm);
			return;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}
	}
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_NPT));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_NESTED_STATE));

	run_test(l1_svm_code_npt_disabled, "nested NPT disabled", false, false);

	run_test(l1_svm_code_invalid_gpat, "invalid g_pat", true, false);

	run_test(l1_svm_code_npt_enabled, "nested NPT enabled", true, false);

	run_test(l1_svm_code_saverestore, "save/restore", true, true);

	run_test(l1_svm_code_multi_vmentry, "multiple entries", true, false);

	return 0;
}
