/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MERMAP_H
#define _LINUX_MERMAP_H

#include <linux/mermap_types.h>
#include <linux/mm.h>

#ifdef CONFIG_MERMAP

#include <asm/mermap.h>

int mermap_mm_prepare(struct mm_struct *mm);
void mermap_mm_init(struct mm_struct *mm);
void mermap_mm_teardown(struct mm_struct *mm);

/* Can the mermap be called from this context? */
static inline bool mermap_ready(void)
{
	return in_task() && current->mm && current->mm->mermap.cpu;
}

struct mermap_alloc *mermap_get(struct page *page, unsigned long size, pgprot_t prot);
void *mermap_get_reserved(struct page *page, pgprot_t prot);
void mermap_put(struct mermap_alloc *alloc);

static inline void *mermap_addr(struct mermap_alloc *alloc)
{
	return (void *)alloc->base;
}

/*
 * arch_mermap_flush_tlb() is called before a part of the local CPU's mermap
 * region is remapped to a new address. No other CPU is allowed to _access_ that
 * region, but the region was mapped there.
 *
 * This may be called with IRQs off.
 *
 * On arm64, this will need to be a broadcast TLB flush. Although the other CPUs
 * are forbidden to access the region, they can leak the data that was mapped
 * there via CPU exploits. Violating break-before-make would mean the data
 * available to these CPU exploits is unpredictable.
 */
extern void arch_mermap_flush_tlb(void);
extern bool arch_mermap_pgprot_allowed(pgprot_t prot);

#if IS_ENABLED(CONFIG_KUNIT)
struct mermap_alloc *__mermap_get(struct mm_struct *mm, struct page *page,
			unsigned long size, pgprot_t prot, bool use_reserve);
void __mermap_put(struct mm_struct *mm, struct mermap_alloc *alloc);
unsigned long mermap_cpu_base(int cpu);
unsigned long mermap_cpu_end(int cpu);
#endif

#else /* CONFIG_MERMAP */

static inline int mermap_mm_prepare(struct mm_struct *mm) { return 0; }
static inline void mermap_mm_init(struct mm_struct *mm) { }
static inline void mermap_mm_teardown(struct mm_struct *mm) { }
static inline bool mermap_ready(void) { return false; }

#endif /* CONFIG_MERMAP */

#endif /* _LINUX_MERMAP_H */
