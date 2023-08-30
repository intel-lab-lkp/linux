// SPDX-License-Identifier: GPL-2.0
/*
 * IA-32 Huge TLB Page Support for Kernel.
 *
 * Copyright (C) 2002, Rohit Seth <rohit.seth@intel.com>
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/err.h>
#include <linux/sysctl.h>
#include <linux/compat.h>
#include <asm/mman.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/elf.h>

/*
 * pmd_huge() returns 1 if @pmd is hugetlb related entry, that is normal
 * hugetlb entry or non-present (migration or hwpoisoned) hugetlb entry.
 * Otherwise, returns 0.
 */
int pmd_huge(pmd_t pmd)
{
	return !pmd_none(pmd) &&
		(pmd_val(pmd) & (_PAGE_PRESENT|_PAGE_PSE)) != _PAGE_PRESENT;
}

/*
 * pud_huge() returns 1 if @pud is hugetlb related entry, that is normal
 * hugetlb entry or non-present (migration or hwpoisoned) hugetlb entry.
 * Otherwise, returns 0.
 */
int pud_huge(pud_t pud)
{
#if CONFIG_PGTABLE_LEVELS > 2
	return !pud_none(pud) &&
		(pud_val(pud) & (_PAGE_PRESENT|_PAGE_PSE)) != _PAGE_PRESENT;
#else
	return 0;
#endif
}

#ifdef CONFIG_HUGETLB_PAGE
static unsigned long hugetlb_get_unmapped_area_bottomup(struct file *file,
		unsigned long addr, unsigned long len,
		unsigned long pgoff, unsigned long flags)
{
	struct hstate *h = hstate_file(file);
	struct vm_unmapped_area_info info;

	info.flags = 0;
	info.length = len;
	info.low_limit = get_mmap_base(1);

	/*
	 * If hint address is above DEFAULT_MAP_WINDOW, look for unmapped area
	 * in the full address space.
	 */
	info.high_limit = in_32bit_syscall() ?
		task_size_32bit() : task_size_64bit(addr > DEFAULT_MAP_WINDOW);

	info.align_mask = PAGE_MASK & ~huge_page_mask(h);
	info.align_offset = 0;
	return vm_unmapped_area(&info);
}

static unsigned long hugetlb_get_unmapped_area_topdown(struct file *file,
		unsigned long addr, unsigned long len,
		unsigned long pgoff, unsigned long flags)
{
	struct hstate *h = hstate_file(file);
	struct vm_unmapped_area_info info;

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = PAGE_SIZE;
	info.high_limit = get_mmap_base(0);

	/*
	 * If hint address is above DEFAULT_MAP_WINDOW, look for unmapped area
	 * in the full address space.
	 */
	if (addr > DEFAULT_MAP_WINDOW && !in_32bit_syscall())
		info.high_limit += TASK_SIZE_MAX - DEFAULT_MAP_WINDOW;

	info.align_mask = PAGE_MASK & ~huge_page_mask(h);
	info.align_offset = 0;
	addr = vm_unmapped_area(&info);

	/*
	 * A failed mmap() very likely causes application failure,
	 * so fall back to the bottom-up function here. This scenario
	 * can happen with large stack limits and large mmap()
	 * allocations.
	 */
	if (addr & ~PAGE_MASK) {
		VM_BUG_ON(addr != -ENOMEM);
		info.flags = 0;
		info.low_limit = TASK_UNMAPPED_BASE;
		info.high_limit = TASK_SIZE_LOW;
		addr = vm_unmapped_area(&info);
	}

	return addr;
}

unsigned long
hugetlb_get_unmapped_area(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct hstate *h = hstate_file(file);
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;

	if (len & ~huge_page_mask(h))
		return -EINVAL;

	if (len > TASK_SIZE)
		return -ENOMEM;

	/* No address checking. See comment at mmap_address_hint_valid() */
	if (flags & MAP_FIXED) {
		if (prepare_hugepage_range(file, addr, len))
			return -EINVAL;
		return addr;
	}

	if (addr) {
		addr &= huge_page_mask(h);
		if (!mmap_address_hint_valid(addr, len))
			goto get_unmapped_area;

		vma = find_vma(mm, addr);
		if (!vma || addr + len <= vm_start_gap(vma))
			return addr;
	}

get_unmapped_area:
	if (mm->get_unmapped_area == arch_get_unmapped_area)
		return hugetlb_get_unmapped_area_bottomup(file, addr, len,
				pgoff, flags);
	else
		return hugetlb_get_unmapped_area_topdown(file, addr, len,
				pgoff, flags);
}

#ifndef CONFIG_HIGHMEM
static void clear_contig_region(struct page *page, unsigned int npages)
{
	clear_pages(page_address(page), npages);
}

/*
 * clear_huge_page(): multi-page clearing variant of clear_huge_page().
 *
 * Taking inspiration from the common code variant, we split the zeroing in
 * three parts: left of the fault, right of the fault, and up to 5 pages
 * in the immediate neighbourhood of the target page.
 *
 * Cleared in that order to keep cache lines of the target region hot.
 *
 * For gigantic pages, there is no expectation of cache locality so we do a
 * straight zeroing.
 */
void clear_huge_page(struct page *page,
		     unsigned long addr_hint, unsigned int pages_per_huge_page)
{
	unsigned long addr = addr_hint &
		~(((unsigned long)pages_per_huge_page << PAGE_SHIFT) - 1);
	const long pgidx = (addr_hint - addr) / PAGE_SIZE;
	const int first_pg = 0, last_pg = pages_per_huge_page - 1;
	const int width = 2; /* pages cleared last on either side */
	int sidx[3], eidx[3];
	int i, n;

	if (pages_per_huge_page > MAX_ORDER_NR_PAGES)
		return clear_contig_region(page, pages_per_huge_page);

	/*
	 * Neighbourhood of the fault. Cleared at the end to ensure
	 * it sticks around in the cache.
	 */
	n = 2;
	sidx[n] = (pgidx - width) < first_pg ? first_pg : (pgidx - width);
	eidx[n] = (pgidx + width) > last_pg  ? last_pg  : (pgidx + width);

	sidx[0] = first_pg;	/* Region to the left of the fault */
	eidx[0] = sidx[n] - 1;

	sidx[1] = eidx[n] + 1;	/* Region to the right of the fault */
	eidx[1] = last_pg;

	for (i = 0; i <= 2; i++) {
		if (eidx[i] >= sidx[i])
			clear_contig_region(page + sidx[i],
					    eidx[i] - sidx[i] + 1);
	}
}
#endif /* CONFIG_HIGHMEM */
#endif /* CONFIG_HUGETLB_PAGE */

#ifdef CONFIG_X86_64
bool __init arch_hugetlb_valid_size(unsigned long size)
{
	if (size == PMD_SIZE)
		return true;
	else if (size == PUD_SIZE && boot_cpu_has(X86_FEATURE_GBPAGES))
		return true;
	else
		return false;
}

#ifdef CONFIG_CONTIG_ALLOC
static __init int gigantic_pages_init(void)
{
	/* With compaction or CMA we can allocate gigantic pages at runtime */
	if (boot_cpu_has(X86_FEATURE_GBPAGES))
		hugetlb_add_hstate(PUD_SHIFT - PAGE_SHIFT);
	return 0;
}
arch_initcall(gigantic_pages_init);
#endif
#endif
