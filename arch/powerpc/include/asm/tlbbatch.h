/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_PPC_TLBBATCH_H
#define _ARCH_PPC_TLBBATCH_H

struct arch_tlbflush_unmap_batch {
	/*
         *
	 */
};

static inline void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
}

static inline void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
						struct mm_struct *mm,
						unsigned long uarddr)
{
}

static inline bool arch_tlbbatch_should_defer(struct mm_struct *mm)
{
	/*ppc always do tlb flush in batch*/
	return false;
}

static inline void arch_flush_tlb_batched_pending(struct mm_struct *mm)
{
}
#endif /* _ARCH_PPC_TLBBATCH_H */
