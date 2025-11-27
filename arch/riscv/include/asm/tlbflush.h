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
static inline unsigned long get_mm_asid(struct mm_struct *mm)
{
	return mm ? cntx2asid(atomic_long_read(&mm->context.id)) : FLUSH_TLB_NO_ASID;
}

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
static inline void local_flush_tlb_page(unsigned long addr)
{
	ALT_SFENCE_VMA_ADDR(addr);
}

static inline void local_flush_tlb_page_asid(unsigned long addr,
					     unsigned long asid)
{
	if (asid != FLUSH_TLB_NO_ASID)
		ALT_SFENCE_VMA_ADDR_ASID(addr, asid);
	else
		local_flush_tlb_page(addr);
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
void flush_pud_tlb_range(struct vm_area_struct *vma, unsigned long start,
			 unsigned long end);
#endif

bool arch_tlbbatch_should_defer(struct mm_struct *mm);
void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
		struct mm_struct *mm, unsigned long start, unsigned long end);
void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch);

extern unsigned long tlb_flush_all_threshold;

#ifdef CONFIG_RISCV_LAZY_TLB_FLUSH

#define MAX_LOADED_MM					6
#define MAX_TLB_FLUSH_TASK				32
#define FLUSH_TLB_ALL_ASID				0x1

struct tlb_context {
	struct mm_struct *mm;
	unsigned int gen;
	bool need_flush;
};

struct tlb_flush_task {
	unsigned long start;
	unsigned long size;
	unsigned long stride;
};

struct tlb_flush_queue {
	atomic_t len;
	unsigned int flag;
	struct tlb_flush_task tasks[MAX_TLB_FLUSH_TASK];
} ____cacheline_aligned_in_smp;

struct tlb_info {
	rwlock_t rwlock;
	struct mm_struct *active_mm;
	unsigned int next_gen;
	struct tlb_context contexts[MAX_LOADED_MM];
	struct tlb_flush_queue *flush_queues;
};

DECLARE_PER_CPU_SHARED_ALIGNED(struct tlb_info, tlbinfo);

void local_load_tlb_mm(struct mm_struct *mm);
void local_flush_tlb_mm(struct mm_struct *mm);
void __init lazy_tlb_flush_init(void);

#else /* CONFIG_RISCV_LAZY_TLB_FLUSH */

static inline void local_load_tlb_mm(struct mm_struct *mm) {}

static inline void local_flush_tlb_mm(struct mm_struct *mm)
{
	local_flush_tlb_all_asid(get_mm_asid(mm));
}

static inline void lazy_tlb_flush_init(void) {}

#endif /* CONFIG_RISCV_LAZY_TLB_FLUSH */

#else /* CONFIG_MMU */
#define local_flush_tlb_all()			do { } while (0)
#endif /* CONFIG_MMU */

#endif /* _ASM_RISCV_TLBFLUSH_H */
