// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM selftest powerpc library code - CPU-related functions (page tables...)
 */

#include <linux/sizes.h>

#include "processor.h"
#include "kvm_util.h"
#include "kvm_util_base.h"
#include "guest_modes.h"
#include "hcall.h"

#define RADIX_TREE_SIZE ((0x2UL << 61) | (0x5UL << 5)) // 52-bits
#define RADIX_PGD_INDEX_SIZE 13

static void set_proc_table(struct kvm_vm *vm, int pid, uint64_t dw0, uint64_t dw1)
{
	uint64_t *proc_table;

	proc_table = addr_gpa2hva(vm, vm->prtb);
	proc_table[pid * 2 + 0] = cpu_to_be64(dw0);
	proc_table[pid * 2 + 1] = cpu_to_be64(dw1);
}

static void set_radix_proc_table(struct kvm_vm *vm, int pid, vm_paddr_t pgd)
{
	set_proc_table(vm, pid, pgd | RADIX_TREE_SIZE | RADIX_PGD_INDEX_SIZE, 0);
}

void virt_arch_pgd_alloc(struct kvm_vm *vm)
{
	struct kvm_ppc_mmuv3_cfg mmu_cfg;
	vm_paddr_t prtb, pgtb;
	size_t pgd_pages;

	TEST_ASSERT((vm->mode == VM_MODE_P52V52_4K) ||
		    (vm->mode == VM_MODE_P52V52_64K),
		    "Unsupported guest mode, mode: 0x%x", vm->mode);

	prtb = vm_phy_page_alloc(vm, KVM_GUEST_PAGE_TABLE_MIN_PADDR,
				 vm->memslots[MEM_REGION_PT]);
	vm->prtb = prtb;

	pgd_pages = (1UL << (RADIX_PGD_INDEX_SIZE + 3)) >> vm->page_shift;
	if (!pgd_pages)
		pgd_pages = 1;
	pgtb = vm_phy_pages_alloc_align(vm, pgd_pages, pgd_pages,
					KVM_GUEST_PAGE_TABLE_MIN_PADDR,
					vm->memslots[MEM_REGION_PT]);
	vm->pgd = pgtb;

	/* Set the base page directory in the proc table */
	set_radix_proc_table(vm, 0, pgtb);

	if (vm->mode == VM_MODE_P52V52_4K)
		mmu_cfg.process_table = prtb | 0x8000000000000000UL | 0x0; // 4K size
	else /* vm->mode == VM_MODE_P52V52_64K */
		mmu_cfg.process_table = prtb | 0x8000000000000000UL | 0x4; // 64K size
	mmu_cfg.flags = KVM_PPC_MMUV3_RADIX | KVM_PPC_MMUV3_GTSE;

	vm_ioctl(vm, KVM_PPC_CONFIGURE_V3_MMU, &mmu_cfg);
}

static int pt_shift(struct kvm_vm *vm, int level)
{
	switch (level) {
	case 1:
		return 13;
	case 2:
	case 3:
		return 9;
	case 4:
		if (vm->mode == VM_MODE_P52V52_4K)
			return 9;
		else /* vm->mode == VM_MODE_P52V52_64K */
			return 5;
	default:
		TEST_ASSERT(false, "Invalid page table level %d\n", level);
		return 0;
	}
}

static uint64_t pt_entry_coverage(struct kvm_vm *vm, int level)
{
	uint64_t size = vm->page_size;

	if (level == 4)
		return size;
	size <<= pt_shift(vm, 4);
	if (level == 3)
		return size;
	size <<= pt_shift(vm, 3);
	if (level == 2)
		return size;
	size <<= pt_shift(vm, 2);
	return size;
}

static int pt_idx(struct kvm_vm *vm, uint64_t vaddr, int level, uint64_t *nls)
{
	switch (level) {
	case 1:
		*nls = 0x9;
		return (vaddr >> 39) & 0x1fff;
	case 2:
		*nls = 0x9;
		return (vaddr >> 30) & 0x1ff;
	case 3:
		if (vm->mode == VM_MODE_P52V52_4K)
			*nls = 0x9;
		else /* vm->mode == VM_MODE_P52V52_64K */
			*nls = 0x5;
		return (vaddr >> 21) & 0x1ff;
	case 4:
		if (vm->mode == VM_MODE_P52V52_4K)
			return (vaddr >> 12) & 0x1ff;
		else /* vm->mode == VM_MODE_P52V52_64K */
			return (vaddr >> 16) & 0x1f;
	default:
		TEST_ASSERT(false, "Invalid page table level %d\n", level);
		return 0;
	}
}

static uint64_t *virt_get_pte(struct kvm_vm *vm, vm_paddr_t pt,
			  uint64_t vaddr, int level, uint64_t *nls)
{
	int idx = pt_idx(vm, vaddr, level, nls);
	uint64_t *ptep = addr_gpa2hva(vm, pt + idx*8);

	return ptep;
}

#define PTE_VALID	0x8000000000000000ull
#define PTE_LEAF	0x4000000000000000ull
#define PTE_REFERENCED	0x0000000000000100ull
#define PTE_CHANGED	0x0000000000000080ull
#define PTE_PRIV	0x0000000000000008ull
#define PTE_READ	0x0000000000000004ull
#define PTE_RW		0x0000000000000002ull
#define PTE_EXEC	0x0000000000000001ull
#define PTE_PAGE_MASK	0x01fffffffffff000ull

#define PDE_VALID	PTE_VALID
#define PDE_NLS		0x0000000000000011ull
#define PDE_PT_MASK	0x0fffffffffffff00ull

static vm_paddr_t __vm_alloc_pt(struct kvm_vm *vm, uint64_t pt_shift)
{
	vm_paddr_t pt;

	if (pt_shift >= vm->page_shift) {
		size_t pt_pages = 1ULL << (pt_shift - vm->page_shift);
		pt = vm_phy_pages_alloc_align(vm, pt_pages, pt_pages,
					KVM_GUEST_PAGE_TABLE_MIN_PADDR,
					vm->memslots[MEM_REGION_PT]);
	} else {
		struct vm_pt_frag_cache *pt_frag_cache;

		if (pt_shift == 8) {
			pt_frag_cache = &vm->pt_frag_cache[0];
		} else if (pt_shift == 12) {
			pt_frag_cache = &vm->pt_frag_cache[1];
		} else {
			TEST_ASSERT(0, "Invalid pt_shift:%lu\n", pt_shift);
			return 0;
		}

		if (!pt_frag_cache->page) {
			pt_frag_cache->page = vm_phy_pages_alloc_align(vm, 1, 1,
						KVM_GUEST_PAGE_TABLE_MIN_PADDR,
						vm->memslots[MEM_REGION_PT]);
		}
		pt = pt_frag_cache->page + pt_frag_cache->page_nr_used;
		pt_frag_cache->page_nr_used += (1 << pt_shift);
		if (pt_frag_cache->page_nr_used == vm->page_size) {
			pt_frag_cache->page = 0;
			pt_frag_cache->page_nr_used = 0;
		}
	}

	return pt;
}

void virt_arch_pg_map(struct kvm_vm *vm, uint64_t gva, uint64_t gpa)
{
	vm_paddr_t pt = vm->pgd;
	uint64_t *ptep, pte;
	int level;

	for (level = 1; level <= 3; level++) {
		uint64_t nls;
		uint64_t *pdep = virt_get_pte(vm, pt, gva, level, &nls);
		uint64_t pde = be64_to_cpu(*pdep);

		if (pde) {
			TEST_ASSERT((pde & PDE_VALID) && !(pde & PTE_LEAF),
				    "Invalid PDE at level: %u gva: 0x%lx pde:0x%lx\n",
				    level, gva, pde);
			pt = pde & PDE_PT_MASK;
			continue;
		}

		pt = __vm_alloc_pt(vm, nls + 3);
		pde = PDE_VALID | nls | pt;
		*pdep = cpu_to_be64(pde);
	}

	ptep = virt_get_pte(vm, pt, gva, level, NULL);
	pte = be64_to_cpu(*ptep);

	TEST_ASSERT(!pte, "PTE already present at level: %u gva: 0x%lx pte:0x%lx\n",
		    level, gva, pte);

	pte = PTE_VALID | PTE_LEAF | PTE_REFERENCED | PTE_CHANGED |PTE_PRIV |
	      PTE_READ | PTE_RW | PTE_EXEC | (gpa & PTE_PAGE_MASK);
	*ptep = cpu_to_be64(pte);
}

vm_paddr_t addr_arch_gva2gpa(struct kvm_vm *vm, vm_vaddr_t gva)
{
	vm_paddr_t pt = vm->pgd;
	uint64_t *ptep, pte;
	int level;

	for (level = 1; level <= 3; level++) {
		uint64_t nls;
		uint64_t *pdep = virt_get_pte(vm, pt, gva, level, &nls);
		uint64_t pde = be64_to_cpu(*pdep);

		TEST_ASSERT((pde & PDE_VALID) && !(pde & PTE_LEAF),
			"PDE not present at level: %u gva: 0x%lx pde:0x%lx\n",
			level, gva, pde);
		pt = pde & PDE_PT_MASK;
	}

	ptep = virt_get_pte(vm, pt, gva, level, NULL);
	pte = be64_to_cpu(*ptep);

	TEST_ASSERT(pte,
		"PTE not present at level: %u gva: 0x%lx pte:0x%lx\n",
		level, gva, pte);

	TEST_ASSERT((pte & PTE_VALID) && (pte & PTE_LEAF) &&
		    (pte & PTE_READ) && (pte & PTE_RW) && (pte & PTE_EXEC),
		    "PTE not valid at level: %u gva: 0x%lx pte:0x%lx\n",
		    level, gva, pte);

	return (pte & PTE_PAGE_MASK) + (gva & (vm->page_size - 1));
}

static void virt_dump_pt(FILE *stream, struct kvm_vm *vm, vm_paddr_t pt,
			 vm_vaddr_t va, int level, uint8_t indent)
{
	int size, idx;

	size = 1U << (pt_shift(vm, level) + 3);

	for (idx = 0; idx < size; idx += 8, va += pt_entry_coverage(vm, level)) {
		uint64_t *page_table = addr_gpa2hva(vm, pt + idx);
		uint64_t pte = be64_to_cpu(*page_table);

		if (!(pte & PTE_VALID))
			continue;

		if (pte & PTE_LEAF) {
			fprintf(stream,
				"%*s PTE[%d] gVA:0x%016lx -> gRA:0x%016llx\n",
				indent, "", idx/8, va, pte & PTE_PAGE_MASK);
		} else {
			fprintf(stream, "%*sPDE%d[%d] gVA:0x%016lx\n",
				indent, "", level, idx/8, va);
			virt_dump_pt(stream, vm, pte & PDE_PT_MASK, va,
				     level + 1, indent + 2);
		}
	}

}

void virt_arch_dump(FILE *stream, struct kvm_vm *vm, uint8_t indent)
{
	vm_paddr_t pt = vm->pgd;

	if (!vm->pgd_created)
		return;

	virt_dump_pt(stream, vm, pt, 0, 1, indent);
}

static unsigned long get_r2(void)
{
	unsigned long r2;

	asm("mr %0,%%r2" : "=r"(r2));

	return r2;
}

struct kvm_vcpu *vm_arch_vcpu_add(struct kvm_vm *vm, uint32_t vcpu_id,
				  void *guest_code)
{
	const size_t stack_size =  SZ_64K;
	vm_vaddr_t stack_vaddr, ex_regs_vaddr;
	vm_paddr_t ex_regs_paddr;
	struct ex_regs *ex_regs;
	struct kvm_regs regs;
	struct kvm_vcpu *vcpu;
	uint64_t lpcr;

	stack_vaddr = __vm_vaddr_alloc(vm, stack_size,
				       DEFAULT_GUEST_STACK_VADDR_MIN,
				       MEM_REGION_DATA);

	ex_regs_vaddr = __vm_vaddr_alloc(vm, stack_size,
				       DEFAULT_GUEST_STACK_VADDR_MIN,
				       MEM_REGION_DATA);
	ex_regs_paddr = addr_gva2gpa(vm, ex_regs_vaddr);
	ex_regs = addr_gpa2hva(vm, ex_regs_paddr);
	ex_regs->vaddr = ex_regs_vaddr;

	vcpu = __vm_vcpu_add(vm, vcpu_id);

	vcpu_enable_cap(vcpu, KVM_CAP_PPC_PAPR, 1);

	/* Setup guest registers */
	vcpu_regs_get(vcpu, &regs);
	vcpu_get_reg(vcpu, KVM_REG_PPC_LPCR_64, &lpcr);

	regs.pc = (uintptr_t)guest_code;
	regs.gpr[1] = stack_vaddr + stack_size - 256;
	regs.gpr[2] = (uintptr_t)get_r2();
	regs.gpr[12] = (uintptr_t)guest_code;
	regs.gpr[13] = (uintptr_t)ex_regs_paddr;

	regs.msr = MSR_SF | MSR_VEC | MSR_VSX | MSR_FP |
		   MSR_ME | MSR_IR | MSR_DR | MSR_RI;

	if (BYTE_ORDER == LITTLE_ENDIAN) {
		regs.msr |= MSR_LE;
		lpcr |= LPCR_ILE;
	} else {
		lpcr &= ~LPCR_ILE;
	}

	vcpu_regs_set(vcpu, &regs);
	vcpu_set_reg(vcpu, KVM_REG_PPC_LPCR_64, lpcr);

	return vcpu;
}

void vcpu_args_set(struct kvm_vcpu *vcpu, unsigned int num, ...)
{
	va_list ap;
	struct kvm_regs regs;
	int i;

	TEST_ASSERT(num >= 1 && num <= 5, "Unsupported number of args: %u\n",
		    num);

	va_start(ap, num);
	vcpu_regs_get(vcpu, &regs);

	for (i = 0; i < num; i++)
		regs.gpr[i + 3] = va_arg(ap, uint64_t);

	vcpu_regs_set(vcpu, &regs);
	va_end(ap);
}

void vcpu_arch_dump(FILE *stream, struct kvm_vcpu *vcpu, uint8_t indent)
{
	struct kvm_regs regs;

	vcpu_regs_get(vcpu, &regs);

	fprintf(stream, "%*sNIA: 0x%016llx  MSR: 0x%016llx\n",
			indent, "", regs.pc, regs.msr);
	fprintf(stream, "%*sLR:  0x%016llx  CTR :0x%016llx\n",
			indent, "", regs.lr, regs.ctr);
	fprintf(stream, "%*sCR:  0x%08llx          XER :0x%016llx\n",
			indent, "", regs.cr, regs.xer);
}

void kvm_arch_vm_post_create(struct kvm_vm *vm)
{
	vm_paddr_t excp_paddr;
	void *mem;

	excp_paddr = vm_phy_page_alloc(vm, 0, vm->memslots[MEM_REGION_DATA]);

	TEST_ASSERT(excp_paddr == 0,
		    "Interrupt vectors not allocated at gPA address 0: (0x%lx)",
		    excp_paddr);

	mem = addr_gpa2hva(vm, excp_paddr);
	memcpy(mem, __interrupts_start, __interrupts_end - __interrupts_start);
}

void assert_on_unhandled_exception(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	if (get_ucall(vcpu, &uc) == UCALL_UNHANDLED) {
		vm_paddr_t ex_regs_paddr;
		struct ex_regs *ex_regs;
		struct kvm_regs regs;

		vcpu_regs_get(vcpu, &regs);
		ex_regs_paddr = (vm_paddr_t)regs.gpr[13];
		ex_regs = addr_gpa2hva(vcpu->vm, ex_regs_paddr);

		TEST_FAIL("Unexpected interrupt in guest NIA:0x%016lx MSR:0x%016lx TRAP:0x%04x",
			  ex_regs->nia, ex_regs->msr, ex_regs->trap);
	}
}

struct handler {
	void (*fn)(struct ex_regs *regs);
	int trap;
};

#define NR_HANDLERS	10
static struct handler handlers[NR_HANDLERS];

void route_interrupt(struct ex_regs *regs)
{
	int i;

	for (i = 0; i < NR_HANDLERS; i++) {
		if (handlers[i].trap == regs->trap) {
			handlers[i].fn(regs);
			return;
		}
	}

	ucall(UCALL_UNHANDLED, 0);
}

void vm_install_exception_handler(struct kvm_vm *vm, int trap,
			       void (*fn)(struct ex_regs *))
{
	int i;

	for (i = 0; i < NR_HANDLERS; i++) {
		if (!handlers[i].trap || handlers[i].trap == trap) {
			if (fn == NULL)
				trap = 0; /* Clear handler */
			handlers[i].trap = trap;
			handlers[i].fn = fn;
			sync_global_to_guest(vm, handlers[i]);
			return;
		}
	}

	TEST_FAIL("Out of exception handlers");
}

void kvm_selftest_arch_init(void)
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_PPC_MMU_RADIX));

	/*
	 * powerpc default mode is set by host page size and not static,
	 * so start by computing that early.
	 */
	guest_modes_append_default();
}
