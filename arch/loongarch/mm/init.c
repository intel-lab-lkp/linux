// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#include <linux/init.h>
#include <linux/export.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/pagemap.h>
#include <linux/memblock.h>
#include <linux/memremap.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/highmem.h>
#include <linux/swap.h>
#include <linux/proc_fs.h>
#include <linux/pfn.h>
#include <linux/hardirq.h>
#include <linux/gfp.h>
#include <linux/hugetlb.h>
#include <linux/mmzone.h>
#include <linux/execmem.h>

#include <asm/asm-offsets.h>
#include <asm/bootinfo.h>
#include <asm/cpu.h>
#include <asm/dma.h>
#include <asm/mmu_context.h>
#include <asm/sections.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>

#define SPAN_NR_ENTRIES(vstart, vend, shift) \
	((((vend) - 1) >> (shift)) - ((vstart) >> (shift)) + 1)

#define NR_BM_PTE_TABLES \
	SPAN_NR_ENTRIES(FIXADDR_START, FIXADDR_TOP, PMD_SHIFT)
#define NR_BM_PMD_TABLES \
	SPAN_NR_ENTRIES(FIXADDR_START, FIXADDR_TOP, PUD_SHIFT)

static_assert(NR_BM_PMD_TABLES == 1);

#define __BM_TABLE_IDX(addr, shift) \
	(((addr) >> (shift)) - (FIXADDR_START >> (shift)))

#define BM_PTE_TABLE_IDX(addr)	__BM_TABLE_IDX(addr, PMD_SHIFT)

static pte_t bm_pte[NR_BM_PTE_TABLES][PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss __maybe_unused;

static inline pte_t *fixmap_pte(unsigned long addr)
{
	return &bm_pte[BM_PTE_TABLE_IDX(addr)][pte_index(addr)];
}

static void __init early_fixmap_init_pte(pmd_t *pmdp, unsigned long addr)
{
	pmd_t pmd = READ_ONCE(*pmdp);
	pte_t *ptep;

	if (!pmd_present(pmd)) {
		ptep = bm_pte[BM_PTE_TABLE_IDX(addr)];
		pmd_populate_kernel(&init_mm, pmdp, ptep);
		kernel_pte_init(ptep);
	}
}

static void __init early_fixmap_init_pmd(pud_t *pudp, unsigned long addr,
					 unsigned long end)
{
	unsigned long next;
	pud_t pud = READ_ONCE(*pudp);
	pmd_t *pmdp;

	if (pud_none(pud))
		pud_populate(&init_mm, pudp, bm_pmd);

	pmdp = pmd_offset(pudp, addr);

#ifndef __PAGETABLE_PMD_FOLDED
	pmd_init(pmdp);
#endif
	do {
		next = pmd_addr_end(addr, end);
		early_fixmap_init_pte(pmdp, addr);
	} while (pmdp++, addr = next, addr != end);
}

static void __init early_fixmap_init_pud(p4d_t *p4dp, unsigned long addr,
					 unsigned long end)
{
	p4d_t p4d = READ_ONCE(*p4dp);
	pud_t *pudp;

#ifndef __PAGETABLE_PUD_FOLDED
	if (CONFIG_PGTABLE_LEVELS > 3 && !p4d_none(p4d) &&
	    p4d_phys(p4d) != __pa_symbol(bm_pud)) {
		/*
		 * We only end up here if the kernel mapping and the fixmap
		 * share the top level pgd entry, which should only happen on
		 * 16k/4 levels configurations.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_PAGE_SIZE_16KB));
	}
#endif

	if (p4d_none(p4d))
		p4d_populate(&init_mm, p4dp, bm_pud);

	pudp = pud_offset(p4dp, addr);

#ifndef __PAGETABLE_PUD_FOLDED
	pud_init(pudp);
#endif
	early_fixmap_init_pmd(pudp, addr, end);
}

/*
 * The p*d_populate functions call virt_to_phys implicitly so they can't be used
 * directly on kernel symbols (bm_p*d). This function is called too early to use
 * lm_alias so __p*d_populate functions must be used to populate with the
 * physical address from __pa_symbol.
 */
void __init early_fixmap_init(void)
{
	unsigned long addr = FIXADDR_START;
	unsigned long end = FIXADDR_TOP;

	pgd_t *pgdp = pgd_offset_k(addr);
	p4d_t *p4dp = p4d_offset(pgdp, addr);

	early_fixmap_init_pud(p4dp, addr, end);
}

unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] __page_aligned_bss;
EXPORT_SYMBOL(empty_zero_page);

void copy_user_highpage(struct page *to, struct page *from,
	unsigned long vaddr, struct vm_area_struct *vma)
{
	void *vfrom, *vto;

	vfrom = kmap_local_page(from);
	vto = kmap_local_page(to);
	copy_page(vto, vfrom);
	kunmap_local(vfrom);
	kunmap_local(vto);
	/* Make sure this page is cleared on other CPU's too before using it */
	smp_wmb();
}

int __ref page_is_ram(unsigned long pfn)
{
	unsigned long addr = PFN_PHYS(pfn);

	return memblock_is_memory(addr) && !memblock_is_reserved(addr);
}

#ifndef CONFIG_NUMA
void __init paging_init(void)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES];

#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = MAX_DMA32_PFN;
#endif
	max_zone_pfns[ZONE_NORMAL] = max_low_pfn;

	free_area_init(max_zone_pfns);
}
#endif /* !CONFIG_NUMA */

void __ref free_initmem(void)
{
	free_initmem_default(POISON_FREE_INITMEM);
}

#ifdef CONFIG_MEMORY_HOTPLUG
int arch_add_memory(int nid, u64 start, u64 size, struct mhp_params *params)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;
	int ret;

	ret = __add_pages(nid, start_pfn, nr_pages, params);

	if (ret)
		pr_warn("%s: Problem encountered in __add_pages() as ret=%d\n",
				__func__,  ret);

	return ret;
}

void arch_remove_memory(u64 start, u64 size, struct vmem_altmap *altmap)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;
	struct page *page = pfn_to_page(start_pfn);

	/* With altmap the first mapped page is offset from @start */
	if (altmap)
		page += vmem_altmap_offset(altmap);
	__remove_pages(start_pfn, nr_pages, altmap);
}

#ifdef CONFIG_NUMA
int memory_add_physaddr_to_nid(u64 start)
{
	return pa_to_nid(start);
}
EXPORT_SYMBOL_GPL(memory_add_physaddr_to_nid);
#endif
#endif

#ifdef CONFIG_SPARSEMEM_VMEMMAP
void __meminit vmemmap_set_pmd(pmd_t *pmd, void *p, int node,
			       unsigned long addr, unsigned long next)
{
	pmd_t entry;

	entry = pfn_pmd(virt_to_pfn(p), PAGE_KERNEL);
	pmd_val(entry) |= _PAGE_HUGE | _PAGE_HGLOBAL;
	set_pmd_at(&init_mm, addr, pmd, entry);
}

int __meminit vmemmap_check_pmd(pmd_t *pmd, int node,
				unsigned long addr, unsigned long next)
{
	int huge = pmd_val(pmdp_get(pmd)) & _PAGE_HUGE;

	if (huge)
		vmemmap_verify((pte_t *)pmd, node, addr, next);

	return huge;
}

int __meminit vmemmap_populate(unsigned long start, unsigned long end,
			       int node, struct vmem_altmap *altmap)
{
#if CONFIG_PGTABLE_LEVELS == 2
	return vmemmap_populate_basepages(start, end, node, NULL);
#else
	return vmemmap_populate_hugepages(start, end, node, NULL);
#endif
}

#ifdef CONFIG_MEMORY_HOTPLUG
void vmemmap_free(unsigned long start, unsigned long end, struct vmem_altmap *altmap)
{
}
#endif
#endif

pte_t * __init populate_kernel_pte(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr);
	p4d_t *p4d = p4d_offset(pgd, addr);
	pud_t *pud;
	pmd_t *pmd;

	if (p4d_none(p4dp_get(p4d))) {
		pud = memblock_alloc_or_panic(PAGE_SIZE, PAGE_SIZE);
		p4d_populate(&init_mm, p4d, pud);
#ifndef __PAGETABLE_PUD_FOLDED
		pud_init(pud);
#endif
	}

	pud = pud_offset(p4d, addr);
	if (pud_none(pudp_get(pud))) {
		pmd = memblock_alloc_or_panic(PAGE_SIZE, PAGE_SIZE);
		pud_populate(&init_mm, pud, pmd);
#ifndef __PAGETABLE_PMD_FOLDED
		pmd_init(pmd);
#endif
	}

	pmd = pmd_offset(pud, addr);
	if (!pmd_present(pmdp_get(pmd))) {
		pte_t *pte;

		pte = memblock_alloc_or_panic(PAGE_SIZE, PAGE_SIZE);
		pmd_populate_kernel(&init_mm, pmd, pte);
		kernel_pte_init(pte);
	}

	return pte_offset_kernel(pmd, addr);
}

void __set_fixmap(enum fixed_addresses idx,
			       phys_addr_t phys, pgprot_t flags)
{
	unsigned long addr = __fix_to_virt(idx);
	pte_t *ptep;

	BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses);

	ptep = fixmap_pte(addr);

	if (pgprot_val(flags))
		set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, flags));
	else {
		pte_clear(&init_mm, addr, ptep);
		flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	}
}

/*
 * Align swapper_pg_dir in to 64K, allows its address to be loaded
 * with a single LUI instruction in the TLB handlers.  If we used
 * __aligned(64K), its size would get rounded up to the alignment
 * size, and waste space.  So we place it in its own section and align
 * it in the linker script.
 */
pgd_t swapper_pg_dir[_PTRS_PER_PGD] __section(".bss..swapper_pg_dir");

pgd_t invalid_pg_dir[_PTRS_PER_PGD] __page_aligned_bss;
#ifndef __PAGETABLE_PUD_FOLDED
pud_t invalid_pud_table[PTRS_PER_PUD] __page_aligned_bss;
EXPORT_SYMBOL(invalid_pud_table);
#endif
#ifndef __PAGETABLE_PMD_FOLDED
pmd_t invalid_pmd_table[PTRS_PER_PMD] __page_aligned_bss;
EXPORT_SYMBOL(invalid_pmd_table);
#endif
pte_t invalid_pte_table[PTRS_PER_PTE] __page_aligned_bss;
EXPORT_SYMBOL(invalid_pte_table);

#ifdef CONFIG_EXECMEM
static struct execmem_info execmem_info __ro_after_init;

struct execmem_info __init *execmem_arch_setup(void)
{
	execmem_info = (struct execmem_info){
		.ranges = {
			[EXECMEM_DEFAULT] = {
				.start	= MODULES_VADDR,
				.end	= MODULES_END,
				.pgprot	= PAGE_KERNEL,
				.alignment = 1,
			},
		},
	};

	return &execmem_info;
}
#endif /* CONFIG_EXECMEM */
