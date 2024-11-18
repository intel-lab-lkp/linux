// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>
#include <asm/kfence.h>

struct page_change_data {
	pgprot_t set_mask;
	pgprot_t clear_mask;
};

bool rodata_full __ro_after_init = IS_ENABLED(CONFIG_RODATA_FULL_DEFAULT_ENABLED);

bool can_set_direct_map(void)
{
	/*
	 * rodata_full and DEBUG_PAGEALLOC require linear map to be
	 * mapped at page granularity, so that it is possible to
	 * protect/unprotect single pages.
	 *
	 * KFENCE pool requires page-granular mapping if initialized late.
	 */
	return rodata_full || debug_pagealloc_enabled() ||
	       arm64_kfence_can_set_direct_map();
}

static int change_page_range(pte_t *ptep, unsigned long addr, void *data)
{
	struct page_change_data *cdata = data;
	pte_t pte = __ptep_get(ptep);

	pte = clear_pte_bit(pte, cdata->clear_mask);
	pte = set_pte_bit(pte, cdata->set_mask);

	__set_pte(ptep, pte);
	return 0;
}

static int __split_linear_mapping_pmd(pud_t *pudp,
				      unsigned long vaddr, unsigned long end)
{
	pmd_t *pmdp;
	unsigned long next;

	pmdp = pmd_offset(pudp, vaddr);

	do {
		next = pmd_addr_end(vaddr, end);

		if (pmd_leaf(pmdp_get(pmdp))) {
			struct page *pte_page;
			unsigned long pfn = pmd_pfn(pmdp_get(pmdp));
			pgprot_t prot = pmd_pgprot(pmdp_get(pmdp));
			pte_t *ptep_new;
			int i;

			pte_page = alloc_page(GFP_KERNEL);
			if (!pte_page)
				return -ENOMEM;

			prot = __pgprot(pgprot_val(prot) | PTE_TYPE_PAGE);
			ptep_new = (pte_t *)page_address(pte_page);
			for (i = 0; i < PTRS_PER_PTE; ++i, ++ptep_new)
				__set_pte_nosync(ptep_new,
						 pfn_pte(pfn + i, prot));

			dsb(ishst);
			isb();

			set_pmd(pmdp, pfn_pmd(page_to_pfn(pte_page),
				__pgprot(PMD_TYPE_TABLE)));
		}
	} while (pmdp++, vaddr = next, vaddr != end);

	return 0;
}

static int __split_linear_mapping_pud(p4d_t *p4dp,
				      unsigned long vaddr, unsigned long end)
{
	pud_t *pudp;
	unsigned long next;
	int ret;

	pudp = pud_offset(p4dp, vaddr);

	do {
		next = pud_addr_end(vaddr, end);

		if (pud_leaf(pudp_get(pudp))) {
			struct page *pmd_page;
			unsigned long pfn = pud_pfn(pudp_get(pudp));
			pgprot_t prot = pud_pgprot(pudp_get(pudp));
			pmd_t *pmdp_new;
			int i;
			unsigned int step;

			pmd_page = alloc_page(GFP_KERNEL);
			if (!pmd_page)
				return -ENOMEM;

			pmdp_new = (pmd_t *)page_address(pmd_page);
			for (i = 0; i < PTRS_PER_PMD; ++i, ++pmdp_new) {
				step = (i * PMD_SIZE) >> PAGE_SHIFT;
				__set_pmd_nosync(pmdp_new,
						 pfn_pmd(pfn + step, prot));
			}

			dsb(ishst);
			isb();

			set_pud(pudp, pfn_pud(page_to_pfn(pmd_page),
				__pgprot(PUD_TYPE_TABLE)));
		}

		ret = __split_linear_mapping_pmd(pudp, vaddr, next);
		if (ret)
			return ret;
	} while (pudp++, vaddr = next, vaddr != end);

	return 0;
}

static int __split_linear_mapping_p4d(pgd_t *pgdp,
				      unsigned long vaddr, unsigned long end)
{
	p4d_t *p4dp;
	unsigned long next;
	int ret;

	p4dp = p4d_offset(pgdp, vaddr);

	do {
		next = p4d_addr_end(vaddr, end);

		ret = __split_linear_mapping_pud(p4dp, vaddr, next);
		if (ret)
			return ret;
	} while (p4dp++, vaddr = next, vaddr != end);

	return 0;
}

static int __split_linear_mapping_pgd(pgd_t *pgdp,
				      unsigned long vaddr,
				      unsigned long end)
{
	unsigned long next;
	int ret = 0;

	mmap_write_lock(&init_mm);

	do {
		next = pgd_addr_end(vaddr, end);
		ret = __split_linear_mapping_p4d(pgdp, vaddr, next);
		if (ret)
			break;
	} while (pgdp++, vaddr = next, vaddr != end);

	mmap_write_unlock(&init_mm);

	return ret;
}

static int split_linear_mapping(unsigned long start, unsigned long end)
{
	int ret;

	if (!system_supports_bbmlv2())
		return 0;

	ret = __split_linear_mapping_pgd(pgd_offset_k(start), start, end);
	flush_tlb_kernel_range(start, end);

	return ret;
}

/*
 * This function assumes that the range is mapped with PAGE_SIZE pages.
 */
static int __change_memory_common(unsigned long start, unsigned long size,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	struct page_change_data data;
	int ret;

	data.set_mask = set_mask;
	data.clear_mask = clear_mask;

	ret = apply_to_page_range(&init_mm, start, size, change_page_range,
					&data);

	flush_tlb_kernel_range(start, start + size);
	return ret;
}

static int change_memory_common(unsigned long addr, int numpages,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	unsigned long start = addr;
	unsigned long size = PAGE_SIZE * numpages;
	unsigned long end = start + size;
	unsigned long l_start;
	struct vm_struct *area;
	int i, ret;

	if (!PAGE_ALIGNED(addr)) {
		start &= PAGE_MASK;
		end = start + size;
		WARN_ON_ONCE(1);
	}

	/*
	 * Kernel VA mappings are always live, and splitting live section
	 * mappings into page mappings may cause TLB conflicts. This means
	 * we have to ensure that changing the permission bits of the range
	 * we are operating on does not result in such splitting.
	 *
	 * Let's restrict ourselves to mappings created by vmalloc (or vmap).
	 * Those are guaranteed to consist entirely of page mappings, and
	 * splitting is never needed.
	 *
	 * So check whether the [addr, addr + size) interval is entirely
	 * covered by precisely one VM area that has the VM_ALLOC flag set.
	 */
	area = find_vm_area((void *)addr);
	if (!area ||
	    end > (unsigned long)kasan_reset_tag(area->addr) + area->size ||
	    !(area->flags & VM_ALLOC))
		return -EINVAL;

	if (!numpages)
		return 0;

	/*
	 * If we are manipulating read-only permissions, apply the same
	 * change to the linear mapping of the pages that back this VM area.
	 */
	if (rodata_full && (pgprot_val(set_mask) == PTE_RDONLY ||
			    pgprot_val(clear_mask) == PTE_RDONLY)) {
		for (i = 0; i < area->nr_pages; i++) {
			l_start = (u64)page_address(area->pages[i]);
			ret = split_linear_mapping(l_start, l_start + PAGE_SIZE);
			if (WARN_ON_ONCE(ret))
				return ret;

			__change_memory_common(l_start,
					       PAGE_SIZE, set_mask, clear_mask);
		}
	}

	/*
	 * Get rid of potentially aliasing lazily unmapped vm areas that may
	 * have permissions set that deviate from the ones we are setting here.
	 */
	vm_unmap_aliases();

	return __change_memory_common(start, size, set_mask, clear_mask);
}

int set_memory_ro(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_RDONLY),
					__pgprot(PTE_WRITE));
}

int set_memory_rw(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_WRITE),
					__pgprot(PTE_RDONLY));
}

int set_memory_nx(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_PXN),
					__pgprot(PTE_MAYBE_GP));
}

int set_memory_x(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_MAYBE_GP),
					__pgprot(PTE_PXN));
}

int set_memory_valid(unsigned long addr, int numpages, int enable)
{
	if (enable)
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					__pgprot(PTE_VALID),
					__pgprot(0));
	else
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					__pgprot(0),
					__pgprot(PTE_VALID));
}

int set_direct_map_invalid_noflush(struct page *page)
{
	unsigned long l_start;
	int ret;

	struct page_change_data data = {
		.set_mask = __pgprot(0),
		.clear_mask = __pgprot(PTE_VALID),
	};

	if (!can_set_direct_map())
		return 0;

	l_start = (unsigned long)page_address(page);
	ret = split_linear_mapping(l_start, l_start + PAGE_SIZE);
	if (WARN_ON_ONCE(ret))
		return ret;

	return apply_to_page_range(&init_mm,
				   l_start, PAGE_SIZE, change_page_range,
				   &data);
}

int set_direct_map_default_noflush(struct page *page)
{
	unsigned long l_start;
	int ret;

	struct page_change_data data = {
		.set_mask = __pgprot(PTE_VALID | PTE_WRITE),
		.clear_mask = __pgprot(PTE_RDONLY),
	};

	if (!can_set_direct_map())
		return 0;

	l_start = (unsigned long)page_address(page);
	ret = split_linear_mapping(l_start, l_start + PAGE_SIZE);
	if (WARN_ON_ONCE(ret))
		return ret;

	return apply_to_page_range(&init_mm,
				   l_start, PAGE_SIZE, change_page_range,
				   &data);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (!can_set_direct_map())
		return;

	set_memory_valid((unsigned long)page_address(page), numpages, enable);
}
#endif /* CONFIG_DEBUG_PAGEALLOC */

/*
 * This function is used to determine if a linear map page has been marked as
 * not-valid. Walk the page table and check the PTE_VALID bit.
 *
 * Because this is only called on the kernel linear map,  p?d_sect() implies
 * p?d_present(). When debug_pagealloc is enabled, sections mappings are
 * disabled.
 */
bool kernel_page_present(struct page *page)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep;
	unsigned long addr = (unsigned long)page_address(page);

	pgdp = pgd_offset_k(addr);
	if (pgd_none(READ_ONCE(*pgdp)))
		return false;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(READ_ONCE(*p4dp)))
		return false;

	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return false;
	if (pud_sect(pud))
		return true;

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return false;
	if (pmd_sect(pmd))
		return true;

	ptep = pte_offset_kernel(pmdp, addr);
	return pte_valid(__ptep_get(ptep));
}
