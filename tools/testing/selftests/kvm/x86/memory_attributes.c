// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * Test for KVM_MEMORY_ATTRIBUTES
 */
#include "kvm_util.h"
#include "apic.h"

uint64_t arch_controlled_read(gva_t addr)
{
	uint64_t val;

	asm volatile("mov %[addr], %%rax \n\r"
		     "mov (%%rax), %[val] \n\r"
		     : [val] "=r" (val)
		     : [addr] "m"(addr)
		     : "memory", "rax");

	return val;
}

void arch_controlled_write(gva_t addr, uint64_t val)
{
	asm volatile("mov %[addr], %%rax \n\r"
		     "mov %[val], %%rbx \n\r"
		     "mov %%rbx, (%%rax) \n\r"
		     :: [addr] "m" (addr), [val] "m" (val)
		     : "memory", "rax", "rbx");
}

void arch_controlled_exec(gva_t addr)
{
	asm volatile("mov %[addr], %%rax \n\r"
		     "call *%%rax \n\t"
		     :: [addr] "m"(addr)
		     : "memory", "rax");
}

void arch_write_return_insn(struct kvm_vm *vm, gpa_t vaddr)
{
	memset(addr_gpa2hva(vm, vaddr), 0xc3, 1);
}

/*
 * This test validates that, during a page walk, if the page a PTE is placed in
 * is read-only the accesss and dirty bits will not be written. Note There's a
 * slight variation in behaviour between TDP and non-TDP VMs:
 *  - With TDP enabled, KVM issues a fault exit upon observing the non-writable
 *  page.
 *  - With non-TDP, the access bit is not set, but the walk succeeds.
 *
 *  This is aligned with read-only memslots' behaviour.
 */
static void test_memory_access_pte_ro(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	struct kvm_vm *vm = vcpu->vm;
	gpa_t paddr;
	u64 *pte;
	const u64 accessed_mask = PTE_ACCESSED_MASK(&vm->mmu);

	pte = vm_get_pte(vm, vaddr);
	paddr = addr_hva2gpa(vm, pte) & GENMASK(61, vm->page_shift);

	*pte &= ~accessed_mask;
	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NW);
	if (test_page(vcpu, TEST_OP_READ, vaddr) < 0) {
		test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr,
				     /* write PTE's accessed bit */
				     KVM_MEMORY_EXIT_FLAG_WRITE);

		vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
		test_page_accessible(vcpu, TEST_OP_READ, vaddr);
		TEST_ASSERT_EQ(*pte & accessed_mask, accessed_mask);

		/* Re-run the test, now vaddr is backed by an EPT. */
		*pte &= ~accessed_mask;
		vm_set_memory_attributes(vm, paddr, vm->page_size,
					 KVM_MEMORY_ATTRIBUTE_NW);
		test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr,
				     /* write PTE's accessed bit */
				     KVM_MEMORY_EXIT_FLAG_WRITE);
		vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
		test_page_accessible(vcpu, TEST_OP_READ, vaddr);
	} else {
		TEST_ASSERT_EQ(*pte & accessed_mask, 0);
		vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	}
}

/*
 * This test validates that, during a page walk, if the page a PTE is placed in
 * is maked as non-accesible, KVM issues a fault exit.
 */
static void test_memory_access_pte_nr(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	struct kvm_vm *vm = vcpu->vm;
	gpa_t paddr;
	uint64_t *pte;

	pte = vm_get_pte(vm, vaddr);
	paddr = addr_hva2gpa(vm, pte) & GENMASK(61, vm->page_shift);

	vm_set_memory_attributes(vm, paddr, vm->page_size,
				 KVM_MEMORY_ATTRIBUTE_NO_ACCESS);

	test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr, 0);

	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	test_page_accessible(vcpu, TEST_OP_READ, vaddr);

	/* Re-run the test, now vaddr is backed by an SPTE. */
	vm_set_memory_attributes(vm, paddr, vm->page_size,
				 KVM_MEMORY_ATTRIBUTE_NO_ACCESS);
	test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr, 0);
	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	test_page_accessible(vcpu, TEST_OP_READ, vaddr);
}

static void test_memory_access_sync_spte(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	struct kvm_vm *vm = vcpu->vm;
	gpa_t paddr = addr_gva2gpa(vm, vaddr);
	uint64_t *pte, old_pte, new_pte;

	pte = vm_get_pte(vm, vaddr);
	gpa_t pte_paddr = addr_hva2gpa(vm, pte);
	gpa_t pte_page_paddr = pte_paddr & GENMASK(61, vm->page_shift);
	int pte_offset = pte_paddr - pte_page_paddr;
	virt_pg_map(vm, PTE_VADDR, pte_page_paddr);
	old_pte = *pte;

	/* Set vmaddr as non-executable */
	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NX);

	/*
	 * Make sure SPTEs are populated as previous op might have destroyed
	 * them. We new have a non-executable SPTE.
	 */
	test_page_accessible(vcpu, TEST_OP_READ, vaddr);

	/*
	 * Update PTE, make it non-writable and flush TLBs to make sure we go
	 * through the sync_spte path. This should update the SPTE and make it
	 * read-only.
	 */
	new_pte = (old_pte & ~PT_WRITABLE_MASK) | PT_ACCESSED_MASK;
	test_data->expected_val = new_pte;
	test_page_accessible(vcpu, TEST_OP_WRITE, PTE_VADDR + pte_offset);
	TEST_ASSERT_EQ(*pte, new_pte);
	test_page_accessible(vcpu, TEST_OP_INVPLG, vaddr);

	/* The not executable attrs remain valid */
	arch_write_return_insn(vm, vaddr);
	test_page_restricted(vcpu, TEST_OP_EXEC, vaddr, paddr,
			     KVM_MEMORY_EXIT_FLAG_EXEC);

	/* Cleanup */
	*pte = old_pte;
	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	test_page_accessible(vcpu, TEST_OP_EXEC, vaddr);
}

static void arch_test_memory_access_pte(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	test_memory_access_pte_nr(vcpu, vaddr);
	test_memory_access_pte_ro(vcpu, vaddr);
	test_memory_access_sync_spte(vcpu, vaddr);
}
