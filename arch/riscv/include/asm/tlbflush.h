/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2009 Chen Liqin <liqin.chen@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 */

#ifndef _ASM_RISCV_TLBFLUSH_H
#define _ASM_RISCV_TLBFLUSH_H

#include <linux/mm_types.h>
#include <asm/smp.h>
#include <asm/errata_list.h>

#define FLUSH_TLB_MAX_SIZE      ((unsigned long)-1)
#define FLUSH_TLB_NO_ASID       ((unsigned long)-1)

#ifdef CONFIG_MMU
static inline void local_flush_tlb_all(void)
{
	__asm__ __volatile__ ("sfence.vma" : : : "memory");
}

static inline void local_flush_tlb_all_asid(unsigned long asid)
{
	if (asid != FLUSH_TLB_NO_ASID)
		ALT_SFENCE_VMA_ASID(asid);
	else
		local_flush_tlb_all();
}

/* Flush one page from local TLB */
static inline void local_flush_tlb_page(unsigned long addr,
					unsigned long page_size)
{
	unsigned int i;
	unsigned long hw_page_num = 1 << (PAGE_SHIFT - HW_PAGE_SHIFT);
	unsigned long hw_page_size = page_size >> (PAGE_SHIFT - HW_PAGE_SHIFT);

	for (i = 0; i < hw_page_num; i++, addr += hw_page_size)
		ALT_SFENCE_VMA_ADDR(addr);
}

static inline void local_flush_tlb_page_asid(unsigned long addr,
					     unsigned long page_size,
					     unsigned long asid)
{
	unsigned int i;
	unsigned long hw_page_num, hw_page_size;

	if (asid != FLUSH_TLB_NO_ASID) {
		hw_page_num = 1 << (PAGE_SHIFT - HW_PAGE_SHIFT);
		hw_page_size = page_size >> (PAGE_SHIFT - HW_PAGE_SHIFT);

		for (i = 0; i < hw_page_num; i++, addr += hw_page_size)
			ALT_SFENCE_VMA_ADDR_ASID(addr, asid);
	} else
		local_flush_tlb_page(addr, page_size);
}

void flush_tlb_all(void);
void flush_tlb_mm(struct mm_struct *mm);
void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
			unsigned long end, unsigned int page_size);
void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr);
void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end);
void flush_tlb_kernel_range(unsigned long start, unsigned long end);
void local_flush_tlb_kernel_range(unsigned long start, unsigned long end);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define __HAVE_ARCH_FLUSH_PMD_TLB_RANGE
void flush_pmd_tlb_range(struct vm_area_struct *vma, unsigned long start,
			unsigned long end);
#endif

bool arch_tlbbatch_should_defer(struct mm_struct *mm);
void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
			       struct mm_struct *mm,
			       unsigned long uaddr);
void arch_flush_tlb_batched_pending(struct mm_struct *mm);
void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch);

extern unsigned long tlb_flush_all_threshold;
#else /* CONFIG_MMU */
#define local_flush_tlb_all()			do { } while (0)
#endif /* CONFIG_MMU */

#endif /* _ASM_RISCV_TLBFLUSH_H */
