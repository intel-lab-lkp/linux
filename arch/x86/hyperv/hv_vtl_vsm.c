// SPDX-License-Identifier: GPL-2.0
/*
 * Architecture-specific bring-up state for the VTL1 secure kernel: build
 * the page tables, GDT/TSS and initial vCPU register context that Hyper-V
 * loads when transitioning the boot processor to VTL1.
 *
 * Copyright (c) 2025-2026, Microsoft Corporation.
 *
 * Author: Thara Gopinath <tgopinath@linux.microsoft.com>
 */

#include <linux/align.h>
#include <linux/bits.h>
#include <linux/init.h>
#include <hyperv/vsm.h>
#include <asm/msr-index.h>
#include <asm/processor-flags.h>
#include <hyperv/hvgdk_mini.h>
#include <asm/mshyperv.h>

/* Define PAGE size and related variables for initial secure kernel pages */
#define VSM_PAGE_SHIFT			12
#define VSM_PAGE_SIZE			BIT(VSM_PAGE_SHIFT)
#define PAGE_AT(addr, idx)		((addr) + (idx) * VSM_PAGE_SIZE)
#define VSM_VA_FROM_PA(pa)		(pa) /* Assumes identity mapping in secure kernel */

/* Number of entries in a page table (all levels) */
#define VSM_ENTRIES_PER_PT		512
#define VSM_PMD_SIZE			(VSM_PAGE_SIZE * VSM_ENTRIES_PER_PT)

/*
 * Initial memory that will be mapped for secure kernel.
 * Secure Kernel memory can be larger than this.
 */
#define VSM_SK_PTE_PAGES_COUNT	(ALIGN(VSM_SK_INITIAL_MAP_SIZE, VSM_PMD_SIZE) / VSM_PMD_SIZE)

/* VSM pages */
enum {
	VSM_GDT_PAGE,
	VSM_TSS_PAGE,
	VSM_PML4E_PAGE,
	VSM_PDPE_PAGE,
	VSM_PDE_PAGE,
	VSM_PTE_PAGES,
	/* PTE tables consume several pages */
	VSM_KERNEL_STACK_PAGE = VSM_PTE_PAGES + VSM_SK_PTE_PAGES_COUNT,
	VSM_PAGES_COUNT
};

#define VSM_PT_FLAGS			(_PAGE_PRESENT | _PAGE_RW)
#define VSM_PTE_FLAGS			(VSM_PT_FLAGS | _PAGE_ACCESSED | _PAGE_DIRTY)

/* Shifts to compute page table mapping */
#define VSM_PD_TABLE_SHIFT		21
#define VSM_PDP_TABLE_SHIFT		30
#define VSM_PML4_TABLE_SHIFT		39

/* Given VA, get index into the page table at a given level */
#define VSM_GET_PML4_INDEX(addr)	(((addr) >> VSM_PML4_TABLE_SHIFT) & 0x1FF)
#define VSM_GET_PDP_INDEX(addr)		(((addr) >> VSM_PDP_TABLE_SHIFT) & 0x1FF)
#define VSM_GET_PD_INDEX(addr)		(((addr) >> VSM_PD_TABLE_SHIFT) & 0x1FF)

static void __init hv_vsm_fill_pte_tables(phys_addr_t sk_pa, u64 *pde,
					  int pd_index, int num_pte_tables)
{
	u16 i, j;
	phys_addr_t pte_pa;
	u64 *pte;

	/* Fill page tables with entries */
	for (i = 0; i < num_pte_tables; i++) {
		pte_pa = PAGE_AT(sk_pa, VSM_PTE_PAGES + i);
		pte = phys_to_virt(pte_pa);
		*(pde + pd_index + i) = pte_pa | VSM_PTE_FLAGS;
		for (j = 0; j < VSM_ENTRIES_PER_PT; j++) {
			*(pte + j) =
				(sk_pa + ((j + (i * VSM_ENTRIES_PER_PT)) * VSM_PAGE_SIZE)) |
					VSM_PTE_FLAGS;
		}
	}
}

static void __init hv_vsm_init_page_tables(struct hv_init_vp_context *vp_ctx, phys_addr_t sk_pa)
{
	unsigned int pml4_index;
	unsigned int pdp_index;
	unsigned int pd_index;
	phys_addr_t pml4e_pa;
	phys_addr_t pdpe_pa;
	phys_addr_t pde_pa;
	u64 *pml4e;
	u64 *pdpe;
	u64 *pde;
	int num_pte_tables;

	/* Compute the page-table indices at which the secure kernel mapping starts. */
	pml4_index = VSM_GET_PML4_INDEX(sk_pa);
	pdp_index = VSM_GET_PDP_INDEX(sk_pa);
	pd_index = VSM_GET_PD_INDEX(sk_pa);

	pml4e_pa = PAGE_AT(sk_pa, VSM_PML4E_PAGE);
	pdpe_pa = PAGE_AT(sk_pa, VSM_PDPE_PAGE);
	pde_pa = PAGE_AT(sk_pa, VSM_PDE_PAGE);

	pml4e = phys_to_virt(pml4e_pa);
	pdpe = phys_to_virt(pdpe_pa);
	pde = phys_to_virt(pde_pa);

	/*
	 * Zero the PML4, PDP, PD and PTE pages before populating them so that
	 * any entry not explicitly written below has its present bit clear.
	 */
	memset(pml4e, 0,
	       (VSM_KERNEL_STACK_PAGE - VSM_PML4E_PAGE) * VSM_PAGE_SIZE);

	*(pml4e + pml4_index) = pdpe_pa | VSM_PT_FLAGS;
	*(pdpe + pdp_index) = pde_pa | VSM_PT_FLAGS;

	/*
	 * Initial page tables map only the first VSM_SK_INITIAL_MAP_SIZE size of memory.
	 * This memory will be used for the Secure Loader and initial Secure Kernel.
	 */
	num_pte_tables = (VSM_SK_INITIAL_MAP_SIZE / VSM_PAGE_SIZE) / VSM_ENTRIES_PER_PT;
	hv_vsm_fill_pte_tables(sk_pa, pde, pd_index, num_pte_tables);

	vp_ctx->cr3 = pml4e_pa;
}

static void __init hv_vsm_init_gdt(struct hv_init_vp_context *vp_ctx, phys_addr_t sk_pa)
{
	phys_addr_t gdt_pa, tss_pa, kstack_pa;
	void *gdt_va;
	u64 tss_sk_va, gdt;
	struct x86_hw_tss *tss;
	size_t gdt_size = sizeof(gdt), tss_size = sizeof(*tss), gdt_offset = 0;

	/* Get a page for the GDT */
	gdt_pa = PAGE_AT(sk_pa, VSM_GDT_PAGE);
	gdt_va = phys_to_virt(gdt_pa);
	/* Get a page for the TSS */
	tss_pa = PAGE_AT(sk_pa, VSM_TSS_PAGE);
	tss = phys_to_virt(tss_pa);
	/* Compute the VA that secure kernel will see for the TSS */
	tss_sk_va = VSM_VA_FROM_PA(tss_pa);
	/* Get a page for the secure kernel initial stack */
	kstack_pa = PAGE_AT(sk_pa, VSM_KERNEL_STACK_PAGE);
	/* Set the initial stack pointer for the kernel to point to bottom of kernel stack */
	tss->sp0 = VSM_VA_FROM_PA(kstack_pa) + VSM_PAGE_SIZE;
	vp_ctx->rsp = tss->sp0;

	/* Make and add the NULL descriptor to the GDT */
	gdt = 0;
	memcpy(gdt_va + gdt_offset, &gdt, gdt_size);
	gdt_offset += gdt_size;

	/* Make and add a code segment descriptor to the GDT */
	gdt = GDT_ENTRY(DESC_CODE64, 0, 0);
	memcpy(gdt_va + gdt_offset, &gdt, gdt_size);
	gdt_offset += gdt_size;

	/* Make and add a data segment descriptor to the GDT */
	gdt = GDT_ENTRY(DESC_DATA64, 0, 0);
	memcpy(gdt_va + gdt_offset, &gdt, gdt_size);
	gdt_offset += gdt_size;

	/*
	 * Make and add a system segment descriptor for the TSS in the GDT.
	 *
	 * In 64-bit mode a system-segment descriptor (TSS/LDT) is 16 bytes
	 * wide: the lower 8 bytes have the same layout as the legacy 32-bit
	 * descriptor (produced by GDT_ENTRY), and the upper 8 bytes hold
	 * base[63:32] in the low 32 bits with the high 32 bits reserved 0.
	 * GDT_ENTRY masks base to 32 bits, so the upper half must be written
	 * explicitly.
	 */
	gdt = GDT_ENTRY(DESC_TSS32, tss_sk_va, tss_size);
	memcpy(gdt_va + gdt_offset, &gdt, gdt_size);
	gdt_offset += gdt_size;
	gdt = tss_sk_va >> 32;
	memcpy(gdt_va + gdt_offset, &gdt, gdt_size);
	gdt_offset += gdt_size;

	/* Set up the GDT register */
	vp_ctx->gdtr.base = VSM_VA_FROM_PA(gdt_pa);
	vp_ctx->gdtr.limit = gdt_offset - 1;

	/* Set the code segment (CS) selector */
	vp_ctx->cs.base = 0;
	vp_ctx->cs.limit = 0;
	vp_ctx->cs.selector = 1 << 3;
	vp_ctx->cs.attributes = _DESC_S | _DESC_PRESENT | _DESC_ACCESSED |
				_DESC_CODE_READABLE | _DESC_CODE_EXECUTABLE |
				_DESC_LONG_CODE | _DESC_GRANULARITY_4K;

	/* Set the data segment (DS) selector */
	vp_ctx->ds.base = 0;
	vp_ctx->ds.limit = 0;
	vp_ctx->ds.selector = 2 << 3;
	vp_ctx->ds.attributes = _DESC_S | _DESC_PRESENT | _DESC_ACCESSED |
				_DESC_DATA_WRITABLE | _DESC_GRANULARITY_4K | _DESC_DB;

	/* Set the ES, FS and GS to be the same as DS, for now */
	vp_ctx->es = vp_ctx->ds;
	vp_ctx->fs = vp_ctx->ds;
	vp_ctx->gs = vp_ctx->ds;

	/* Set the stack selector to 0 (unused in long mode) */
	vp_ctx->ss.selector = 0;

	/* Set the task register selector */
	vp_ctx->tr.base = tss_sk_va;
	vp_ctx->tr.limit = tss_size - 1;
	vp_ctx->tr.selector = 3 << 3;
	vp_ctx->tr.attributes = _DESC_PRESENT | _DESC_SYSTEM(11);
}

static void __init hv_vsm_init_cpu(struct hv_init_vp_context *vp_ctx, Elf64_Addr sk_entry_pa)
{
	/* Offset rip by any secure kernel header length */
	vp_ctx->rip = VSM_VA_FROM_PA(sk_entry_pa);

	/* ToDo: Check if can be replaced with CR0_STATE */
	vp_ctx->cr0 =
		X86_CR0_PG |	/* Paging */
		X86_CR0_WP |	/* Write Protect */
		X86_CR0_NE |	/* Numeric Error */
		X86_CR0_ET |	/* Extension Type */
		X86_CR0_MP |	/* Math Present */
		X86_CR0_PE;	/* Protection Enable */

	vp_ctx->cr4 =
		X86_CR4_PSE |	/* Page Size Extensions */
		X86_CR4_PGE |	/* Page Global Enable */
		X86_CR4_PAE;	/* Physical Address Extensions */

	vp_ctx->efer =
		EFER_LMA |	/* Long Mode Active */
		EFER_LME |	/* Long Mode Enable */
		EFER_NX  |	/* No Execute Enable */
		EFER_SCE;	/* System Call Enable */

	/*
	 * Intel CPUs fail if the architectural read-as-one bit 1 of RFLAGS is not
	 * set. See Intel SDM Vol 3C, 26.3.1.4 (RFLAGS).
	 *
	 * TODO: Has Hyper-V implemented setting this automatically?
	 */
	vp_ctx->rflags = X86_EFLAGS_FIXED;

	vp_ctx->msr_cr_pat = PAT_VALUE(WB, WT, UC_MINUS, UC, WB, WT, UC_MINUS, UC);
}

void __init hv_vsm_arch_init_vp(struct hv_init_vp_context *vp_ctx, Elf64_Addr sk_entry_pa,
				phys_addr_t sk_pa)
{
	hv_vsm_init_cpu(vp_ctx, sk_entry_pa);
	hv_vsm_init_gdt(vp_ctx, sk_pa);
	hv_vsm_init_page_tables(vp_ctx, sk_pa);
}

/* Implemented in mshv_vtl_asm.S */
void __hv_vsm_vtlcall(struct hv_vtlcall_param *args);

DEFINE_STATIC_CALL_NULL(__hv_vsm_vtlcall_hypercall, void (*)(void));

void __init hv_vsm_init_vtlcall(u64 vtl_call_offset)
{
	static_call_update(__hv_vsm_vtlcall_hypercall,
			   (void *)((u8 *)hv_hypercall_pg + vtl_call_offset));
}

s64 hv_vsm_vtlcall(struct hv_vtlcall_param *args)
{
	unsigned long flags;
	u64 cr2;

	local_irq_save(flags);
	kernel_fpu_begin_mask(0);
	cr2 = native_read_cr2();
	__hv_vsm_vtlcall(args);
	native_write_cr2(cr2);
	kernel_fpu_end();
	local_irq_restore(flags);

	/* The secure kernel returns a signed 64-bit status in a3. */
	return (s64)args->a3;
}
