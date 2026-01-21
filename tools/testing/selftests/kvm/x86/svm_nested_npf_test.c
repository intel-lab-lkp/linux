// SPDX-License-Identifier: GPL-2.0-only
/*
 * svm_nested_npf_test
 *
 * Test nested NPF injection when the original VM exit was not an NPF.
 * This exercises nested_svm_inject_npf_exit() with exit_code != SVM_EXIT_NPF.
 *
 * L2 executes OUTS with the source address mapped in L2's page tables but
 * not in L1's NPT. KVM emulates the string I/O instruction, and when it
 * tries to read the source operand, the GPA->HPA translation fails. KVM
 * then injects an NPF to L1 even though the original exit was IOIO.
 *
 * Test 1: Final data page GPA not in NPT (PFERR_GUEST_FINAL_MASK)
 * Test 2: Page table page GPA not in NPT (PFERR_GUEST_PAGE_MASK)
 *
 * Copyright (C) 2025, Google, Inc.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"

#define L2_GUEST_STACK_SIZE 64

enum test_type {
	TEST_FINAL_PAGE_UNMAPPED, /* Final data page GPA not in NPT */
	TEST_PT_PAGE_UNMAPPED, /* Page table page GPA not in NPT */
};

static void *l2_test_page;

#define TEST_IO_PORT 0x80
#define TEST1_VADDR 0x8000000ULL
#define TEST2_VADDR 0x10000000ULL

/*
 * L2 executes OUTS with source at l2_test_page, triggering a nested NPF.
 * The address is mapped in L2's page tables, but either the data page or
 * a PT page is unmapped from L1's NPT, causing the fault.
 */
static void l2_guest_code(void *unused)
{
	asm volatile("outsb" ::"S"(l2_test_page), "d"(TEST_IO_PORT) : "memory");
	GUEST_ASSERT(0);
}

static void l1_guest_code(struct svm_test_data *svm, void *expected_fault_gpa,
						  uint64_t exit_info_1_mask)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb = svm->vmcb;

	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	run_guest(vmcb, svm->vmcb_gpa);

	/* Verify we got an NPF exit (converted from IOIO by KVM) */
	__GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_NPF,
		       "Expected NPF exit (0x%x), got 0x%lx", SVM_EXIT_NPF,
		       vmcb->control.exit_code);

	/* Check for PFERR_GUEST_FINAL_MASK or PFERR_GUEST_PAGE_MASK */
	__GUEST_ASSERT(vmcb->control.exit_info_1 & exit_info_1_mask,
		       "Expected exit_info_1 to have 0x%lx set, got 0x%lx",
		       (unsigned long)exit_info_1_mask,
		       (unsigned long)vmcb->control.exit_info_1);

	__GUEST_ASSERT(vmcb->control.exit_info_2 == (u64)expected_fault_gpa,
		       "Expected exit_info_2 = 0x%lx, got 0x%lx",
		       (unsigned long)expected_fault_gpa,
		       (unsigned long)vmcb->control.exit_info_2);

	GUEST_DONE();
}

/* Returns the GPA of the PT page that maps @vaddr. */
static uint64_t get_pt_gpa_for_vaddr(struct kvm_vm *vm, uint64_t vaddr)
{
	uint64_t *pte;

	pte = vm_get_pte(vm, vaddr);
	TEST_ASSERT(pte && (*pte & 0x1), "PTE not present for vaddr 0x%lx",
		    (unsigned long)vaddr);

	return addr_hva2gpa(vm, (void *)((uint64_t)pte & ~0xFFFULL));
}

static void run_test(enum test_type type)
{
	vm_paddr_t expected_fault_gpa;
	uint64_t exit_info_1_mask;
	vm_vaddr_t svm_gva;

	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
	vm_enable_npt(vm);
	vcpu_alloc_svm(vm, &svm_gva);

	if (type == TEST_FINAL_PAGE_UNMAPPED) {
		/*
		 * Test 1: Unmap the final data page from NPT. The page table
		 * walk succeeds, but the final GPA->HPA translation fails.
		 */
		l2_test_page =
			(void *)vm_vaddr_alloc(vm, vm->page_size, TEST1_VADDR);
		expected_fault_gpa = addr_gva2gpa(vm, (vm_vaddr_t)l2_test_page);
		exit_info_1_mask = PFERR_GUEST_FINAL_MASK;
	} else {
		/*
		 * Test 2: Unmap a PT page from NPT. The hardware page table
		 * walk fails when translating the PT page's GPA through NPT.
		 */
		l2_test_page =
			(void *)vm_vaddr_alloc(vm, vm->page_size, TEST2_VADDR);
		expected_fault_gpa =
			get_pt_gpa_for_vaddr(vm, (vm_vaddr_t)l2_test_page);
		exit_info_1_mask = PFERR_GUEST_PAGE_MASK;
	}

	tdp_identity_map_default_memslots(vm);
	tdp_unmap(vm, expected_fault_gpa, vm->page_size);

	sync_global_to_guest(vm, l2_test_page);
	vcpu_args_set(vcpu, 3, svm_gva, expected_fault_gpa, exit_info_1_mask);

	vcpu_run(vcpu);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_DONE:
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
	default:
		TEST_FAIL("Unexpected exit reason: %d", vcpu->run->exit_reason);
	}

	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));
	TEST_REQUIRE(kvm_cpu_has_npt());

	run_test(TEST_FINAL_PAGE_UNMAPPED);
	run_test(TEST_PT_PAGE_UNMAPPED);

	return 0;
}
