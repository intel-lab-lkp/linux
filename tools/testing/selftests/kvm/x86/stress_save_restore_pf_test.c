// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#define NR_ITERATIONS		500

#define CURSOR_UP		"\033[A"
#define PRINT_ITER(s, x)					\
({								\
	printf("%s\r%s%d\n", (x ? CURSOR_UP : ""), s, x);	\
	fflush(stdout);						\
})

#define TEST_MEM_BASE		0xc0000000ULL
#define NR_TEST_ADDRS		512
#define PATTERN			0xabcdefabcdefabcdULL

#define PTRS_PER_PTE		512
#define PXD_INDEX(vaddr, level)	(((vaddr) >> PG_LEVEL_SHIFT(level)) & (PTRS_PER_PTE - 1))

static u64 pte_present_mask;
static u64 pte_huge_mask;

static u64 expected_vaddr;
static u64 guest_accesses;

static u64 *guest_get_pte(u64 vaddr)
{
	u64 *pgd, *p4d, *pud, *pmd, *pte;
	u64 pgde, p4de, pude, pmde;
	bool la57;

	la57 = !!(get_cr4() & X86_CR4_LA57);
	pgd = (u64 *)(get_cr3() & PHYSICAL_PAGE_MASK);

	if (la57) {
		pgde = pgd[PXD_INDEX(vaddr, PG_LEVEL_256T)];
		GUEST_ASSERT(pgde & pte_present_mask);
		p4d = (u64 *)PTE_GET_PA(pgde);
		p4de = p4d[PXD_INDEX(vaddr, PG_LEVEL_512G)];
	} else {
		pgde = pgd[PXD_INDEX(vaddr, PG_LEVEL_512G)];
		p4de = pgde;
	}

	GUEST_ASSERT(p4de & pte_present_mask);
	pud = (u64 *)PTE_GET_PA(p4de);

	pude = pud[PXD_INDEX(vaddr, PG_LEVEL_1G)];
	GUEST_ASSERT(pude & pte_present_mask);
	GUEST_ASSERT(!(pude & pte_huge_mask));
	pmd = (u64 *)PTE_GET_PA(pude);

	pmde = pmd[PXD_INDEX(vaddr, PG_LEVEL_2M)];
	GUEST_ASSERT(pmde & pte_present_mask);
	GUEST_ASSERT(!(pmde & pte_huge_mask));
	pte = (u64 *)PTE_GET_PA(pmde);

	return &pte[PXD_INDEX(vaddr, PG_LEVEL_4K)];
}

static void guest_pf_handler(struct ex_regs *regs)
{
	u64 fault_addr;
	u64 *ptep;

	fault_addr = get_cr2();
	GUEST_ASSERT_EQ(fault_addr, READ_ONCE(expected_vaddr));

	ptep = guest_get_pte(fault_addr);
	GUEST_ASSERT(ptep);
	GUEST_ASSERT(!(*ptep & pte_present_mask));

	*ptep |= pte_present_mask;
	invlpg(fault_addr);
}

static void guest_access_memory(void *arg)
{
	u64 vaddr, val;

	for (;; guest_accesses++) {
		vaddr = TEST_MEM_BASE + (guest_accesses % NR_TEST_ADDRS) * PAGE_SIZE;
		WRITE_ONCE(expected_vaddr, vaddr);

		/* Read to trigger #PF */
		val = READ_ONCE(*(u64 *)vaddr);
		GUEST_ASSERT_EQ(val, PATTERN);

		/* Clear the present bit again so it faults next time */
		*guest_get_pte(vaddr) &= ~pte_present_mask;
		invlpg(vaddr);

		GUEST_SYNC(guest_accesses);
	}
}

int main(int argc, char *argv[])
{
	struct kvm_x86_state *state;
	struct kvm_vcpu *vcpu;
	int r, i, count = 0;
	struct kvm_vm *vm;
	struct ucall uc;
	gva_t gva;
	gpa_t gpa;

	vm = vm_create_with_one_vcpu(&vcpu, guest_access_memory);
	vm_install_exception_handler(vm, PF_VECTOR, guest_pf_handler);

	pte_present_mask = PTE_PRESENT_MASK(&vm->mmu);
	pte_huge_mask = PTE_HUGE_MASK(&vm->mmu);
	sync_global_to_guest(vm, pte_present_mask);
	sync_global_to_guest(vm, pte_huge_mask);

	/* Allocate a page and write the pattern to it */
	gva = vm_alloc_page(vm);
	*(u64 *)addr_gva2hva(vm, gva) = PATTERN;
	gpa = addr_gva2gpa(vm, gva);

	/*
	 * Map all virtual addresses to the pattern page and clear the present
	 * bit such that guest accesses will cause a #PF.
	 */
	for (i = 0; i < NR_TEST_ADDRS; i++) {
		gva = TEST_MEM_BASE + i * getpagesize();
		virt_pg_map(vm, gva, gpa);
		*vm_get_pte(vm, gva) &= ~pte_present_mask;
	}

	/* Map the page tables so that the guest #PF handler can walk them */
	virt_map_page_tables(vm);

	while (count++ < NR_ITERATIONS) {
		r = __vcpu_run(vcpu);
		TEST_ASSERT(!r, "vcpu_run failed");
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		get_ucall(vcpu, &uc);
		if (uc.cmd == UCALL_ABORT) {
			REPORT_GUEST_ASSERT(uc);
			break;
		}
		TEST_ASSERT_EQ(uc.cmd, UCALL_SYNC);
		TEST_ASSERT_EQ(uc.args[1], count - 1);

		state = vcpu_save_state(vcpu);

		kvm_vm_release(vm);
		vcpu = vm_recreate_with_one_vcpu(vm);
		vcpu_load_state(vcpu, state);
		kvm_x86_state_cleanup(state);

		PRINT_ITER("Save+restore iterations: ", count);
	}

	sync_global_from_guest(vm, guest_accesses);
	pr_info("Guest page accesses: %lu\n", guest_accesses);

	kvm_vm_free(vm);
	return 0;
}
