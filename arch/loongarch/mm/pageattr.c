// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */

#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/set_memory.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>

static unsigned long set_pageattr_masks(struct cpa_data *cpa, unsigned long val)
{
	val &= ~pgprot_val(cpa->mask_clr);
	val |= pgprot_val(cpa->mask_set);

	return val;
}

int arch_should_split_large_page(struct cpa_data *cpa, struct cpa_split_data *sd)
{
	pte_t old = ptep_get(sd->kpte);
	unsigned long val = pte_val(old);
	unsigned long new_val = set_pageattr_masks(cpa, val);

	cpa->pfn = pte_pfn(old) + ((sd->address & (sd->psize - 1)) >> PAGE_SHIFT);

	if (new_val != val) {
		set_pte(sd->kpte, __pte(new_val));
		cpa->flags |= CPA_FLUSHTLB;
	}

	return 0;
}

int arch_split_large_page(struct cpa_data *cpa, struct cpa_split_data *sd)
{
	WARN_ON_ONCE(1);

	return -EINVAL;
}

void arch_change_pte(struct cpa_data *cpa, unsigned long address,
		     pte_t *kpte, pte_t old_pte, bool nx, bool rw)
{
	unsigned long val = pte_val(old_pte);
	pte_t new_pte = __pte(set_pageattr_masks(cpa, val));

	cpa->pfn = pte_pfn(old_pte);

	if (pte_val(old_pte) != pte_val(new_pte)) {
		set_pte(kpte, new_pte);
		cpa->flags |= CPA_FLUSHTLB;
	}
}

int arch_cpa_process_fault(struct cpa_data *cpa, unsigned long vaddr,
			   int primary)
{
	/*
	 * The direct map has no page tables and vmalloc ranges may contain
	 * holes. Both are silently skipped.
	 */
	cpa->numpages = 1;

	return 0;
}

int arch_cpa_process_alias(struct cpa_data *cpa)
{
	return 0;
}

void arch_cpa_flush(struct cpa_data *cpa, int err)
{
	if (err || cpa->force_flush_all ||
	    (cpa->flags & (CPA_ARRAY | CPA_PAGES_ARRAY))) {
		flush_tlb_all();
		return;
	}

	flush_tlb_kernel_range(*cpa->vaddr, *cpa->vaddr + cpa->numpages * PAGE_SIZE);
}

int set_memory_x(unsigned long addr, int numpages)
{
	if (addr < vm_map_base)
		return 0;

	return change_page_attr_clear(&addr, numpages, __pgprot(_PAGE_NO_EXEC), 0);
}

int set_memory_nx(unsigned long addr, int numpages)
{
	if (addr < vm_map_base)
		return 0;

	return change_page_attr_set(&addr, numpages, __pgprot(_PAGE_NO_EXEC), 0);
}

int set_memory_ro(unsigned long addr, int numpages)
{
	if (addr < vm_map_base)
		return 0;

	return change_page_attr_clear(&addr, numpages, __pgprot(_PAGE_WRITE | _PAGE_DIRTY), 0);
}

int set_memory_rw(unsigned long addr, int numpages)
{
	if (addr < vm_map_base)
		return 0;

	return change_page_attr_set(&addr, numpages, __pgprot(_PAGE_WRITE | _PAGE_DIRTY), 0);
}

bool kernel_page_present(struct page *page)
{
	unsigned int level;
	unsigned long addr = (unsigned long)page_address(page);
	pte_t *pte;

	if (addr < vm_map_base)
		return memblock_is_memory(__pa(addr));

	pte = lookup_address(addr, &level);
	if (!pte)
		return false;

	return pte_present(ptep_get(pte));
}

int set_direct_map_default_noflush(struct page *page)
{
	unsigned long addr = (unsigned long)page_address(page);
	struct cpa_data cpa = { .vaddr = &addr,
				.pgd = NULL,
				.numpages = 1,
				.mask_set = PAGE_KERNEL,
				.mask_clr = __pgprot(0),
				.flags = CPA_NO_CHECK_ALIAS };

	if (addr < vm_map_base)
		return 0;

	return __change_page_attr_set_clr(&cpa, 1);
}

int set_direct_map_invalid_noflush(struct page *page)
{
	unsigned long addr = (unsigned long)page_address(page);
	struct cpa_data cpa = { .vaddr = &addr,
				.pgd = NULL,
				.numpages = 1,
				.mask_set = __pgprot(0),
				.mask_clr = __pgprot(_PAGE_PRESENT | _PAGE_VALID),
				.flags = CPA_NO_CHECK_ALIAS };

	if (addr < vm_map_base)
		return 0;

	return __change_page_attr_set_clr(&cpa, 1);
}

int set_direct_map_valid_noflush(struct page *page, unsigned nr, bool valid)
{
	unsigned long addr = (unsigned long)page_address(page);
	struct cpa_data cpa = { .vaddr = &addr,
				.pgd = NULL,
				.numpages = nr,
				.flags = CPA_NO_CHECK_ALIAS };

	if (addr < vm_map_base)
		return 0;

	if (valid) {
		cpa.mask_set = PAGE_KERNEL;
		cpa.mask_clr = __pgprot(0);
	} else {
		cpa.mask_set = __pgprot(0);
		cpa.mask_clr = __pgprot(_PAGE_PRESENT | _PAGE_VALID);
	}

	return __change_page_attr_set_clr(&cpa, 1);
}
