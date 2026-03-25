// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 Nested virtualization helpers, nested page table code adapted from
 * ../x86/vmx.c.
 */

#include <linux/sizes.h>

#include "kvm_util.h"
#include "nested.h"
#include "processor.h"
#include "test_util.h"

#include <asm/sysreg.h>

static void hvc_handler(struct ex_regs *regs)
{
	GUEST_ASSERT_EQ(get_current_el(), 2);
	GUEST_PRINTF("hvc handler\n");
	regs->pstate = PSR_MODE_EL2h | PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;
	regs->pc = (u64)after_hvc;
}

uint64_t get_l1_vtcr(void)
{
	return VTCR_EL2_PS_40_BITS | VTCR_EL2_TG0_4K | VTCR_EL2_ORGN0_WBWA |
	       VTCR_EL2_IRGN0_WBWA | VTCR_EL2_SL0_LV0_4K | VTCR_EL2_T0SZ_BITS(48);
}

static void __nested_pg_map(struct kvm_vm *vm, uint64_t guest_pgd,
		     uint64_t nested_paddr, uint64_t paddr, uint64_t flags)
{
	uint8_t attr_idx = flags & (PTE_ATTRINDX_MASK >> PTE_ATTRINDX_SHIFT);
	uint64_t pg_attr;
	uint64_t *ptep;

	TEST_ASSERT((nested_paddr % vm->page_size) == 0,
		"L2 IPA not on page boundary,\n"
		"  nested_paddr: 0x%lx vm->page_size: 0x%x", nested_paddr, vm->page_size);
	TEST_ASSERT((paddr % vm->page_size) == 0,
		"Guest physical address not on page boundary,\n"
		"  paddr: 0x%lx vm->page_size: 0x%x", paddr, vm->page_size);
	TEST_ASSERT((paddr >> vm->page_shift) <= vm->max_gfn,
		"Physical address beyond maximum supported,\n"
		"  paddr: 0x%lx vm->max_gfn: 0x%lx vm->page_size: 0x%x",
		paddr, vm->max_gfn, vm->page_size);

	ptep = addr_gpa2hva(vm, guest_pgd) + ((nested_paddr >> 39) & 0x1ffu) * 8;
	if (!*ptep)
		*ptep = (vm_alloc_page_table(vm) & GENMASK(47, 12)) | PGD_TYPE_TABLE | PTE_VALID;
	ptep = addr_gpa2hva(vm, *ptep & GENMASK(47, 12)) + ((nested_paddr >> 30) & 0x1ffu) * 8;
	if (!*ptep)
		*ptep = (vm_alloc_page_table(vm) & GENMASK(47, 12)) | PUD_TYPE_TABLE | PTE_VALID;
	ptep = addr_gpa2hva(vm, *ptep & GENMASK(47, 12)) + ((nested_paddr >> 21) & 0x1ffu) * 8;
	if (!*ptep)
		*ptep = (vm_alloc_page_table(vm) & GENMASK(47, 12)) | PMD_TYPE_TABLE | PTE_VALID;
	ptep = addr_gpa2hva(vm, *ptep & GENMASK(47, 12)) + ((nested_paddr >> 12) & 0x1ffu) * 8;

	pg_attr = PTE_AF | PTE_ATTRINDX(attr_idx) | PTE_TYPE_PAGE | PTE_VALID;
	pg_attr |= PTE_SHARED;

	*ptep = (paddr & GENMASK(47, 12)) | pg_attr;
}

void nested_map(struct kvm_vm *vm, vm_paddr_t guest_pgd,
		uint64_t nested_paddr, uint64_t paddr, uint64_t size)
{
	size_t npages = size / SZ_4K;

	TEST_ASSERT(nested_paddr + size > nested_paddr, "Vaddr overflow");
	TEST_ASSERT(paddr + size > paddr, "Paddr overflow");

	while (npages--) {
		__nested_pg_map(vm, guest_pgd, nested_paddr, paddr, MT_NORMAL);
		nested_paddr += SZ_4K;
		paddr += SZ_4K;
	}
}

/*
 * Prepare an identity shadow page table that maps all the
 * physical pages in VM.
 */
void nested_map_memslot(struct kvm_vm *vm, vm_paddr_t guest_pgd,
			uint32_t memslot)
{
	sparsebit_idx_t i, last;
	struct userspace_mem_region *region =
		memslot2region(vm, memslot);

	i = (region->region.guest_phys_addr >> vm->page_shift) - 1;
	last = i + (region->region.memory_size >> vm->page_shift);
	for (;;) {
		i = sparsebit_next_clear(region->unused_phy_pages, i);
		if (i > last)
			break;

		nested_map(vm, guest_pgd,
			   (uint64_t)i << vm->page_shift,
			   (uint64_t)i << vm->page_shift,
			   1 << vm->page_shift);
	}
}

void prepare_l2_stack(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	size_t l2_stack_size;
	uint64_t l2_stack_paddr;

	l2_stack_size = vm->page_size == 4096 ? DEFAULT_STACK_PGS * vm->page_size :
					 vm->page_size;
	l2_stack_paddr = __vm_phy_pages_alloc(vm, l2_stack_size / vm->page_size,
					      0, 0, false);
	vcpu_set_reg(vcpu, ARM64_CORE_REG(sp_el1), l2_stack_paddr + l2_stack_size);
}

void prepare_hyp_state(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	vm_paddr_t guest_pgd;

	guest_pgd = vm_phy_pages_alloc(vm, 1,
				       KVM_GUEST_PAGE_TABLE_MIN_PADDR,
				       vm->memslots[MEM_REGION_PT]);
	nested_map_memslot(vm, guest_pgd, 0);

	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_HCR_EL2), HCR_EL2_RW | HCR_EL2_VM);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_VTTBR_EL2), guest_pgd);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_VTCR_EL2), get_l1_vtcr());
}

void prepare_eret_destination(struct kvm_vm *vm, struct kvm_vcpu *vcpu, void *l2_pc)
{
	vm_paddr_t do_hvc_paddr = addr_gva2gpa(vm, (vm_vaddr_t)do_hvc);
	vm_paddr_t l2_pc_paddr = addr_gva2gpa(vm, (vm_vaddr_t)l2_pc);

	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_SPSR_EL2), PSR_MODE_EL1h |
							    PSR_D_BIT     |
							    PSR_A_BIT     |
							    PSR_I_BIT     |
							    PSR_F_BIT);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_ELR_EL2), l2_pc_paddr);
	/* HACK: use TPIDR_EL2 to pass address, see run_l2() in nested_asm.S */
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_TPIDR_EL2), do_hvc_paddr);
}

void prepare_nested_sync_handler(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	if (!vm->handlers) {
		vm_init_descriptor_tables(vm);
		vcpu_init_descriptor_tables(vcpu);
	}
	vm_install_sync_handler(vm, VECTOR_SYNC_LOWER_64,
				ESR_ELx_EC_HVC64, hvc_handler);
}
