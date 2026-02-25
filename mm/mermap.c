// SPDX-License-Identifier: GPL-2.0
#include <linux/io.h>
#include <linux/mermap.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/pgtable.h>
#include <linux/sched.h>

#include <kunit/visibility.h>

/*
 * As a hack to allow using apply_to_existing_page_range() for these mappings,
 * which skips pte_none() entries, unmap using a special non-"none" sentinel
 * value.
 */
static inline int set_unmapped_pte(pte_t *ptep, unsigned long addr, void *data)
{
	pte_t pte = pfn_pte(0, pgprot_nx(PAGE_NONE));

	VM_BUG_ON(pte_none(pte));
	set_pte(ptep, pte);
	return 0;
}

static void __mermap_put(struct mm_struct *mm, struct mermap_alloc *alloc)
{
	unsigned long size = PAGE_ALIGN(alloc->end - alloc->base);

	if (WARN_ON_ONCE(!alloc->in_use))
		return;

	apply_to_page_range(mm, alloc->base, size, set_unmapped_pte, NULL);

	WRITE_ONCE(alloc->in_use, false);

	migrate_enable();
}

/* Return a region allocated by mermap_get(). */
void mermap_put(struct mermap_alloc *alloc)
{
	__mermap_put(current->mm, alloc);
}
EXPORT_SYMBOL(mermap_put);

static inline unsigned long mermap_cpu_base(int cpu)
{
	return MERMAP_BASE_ADDR + (cpu * MERMAP_CPU_REGION_SIZE);

}

/* Non-inclusive */
static inline unsigned long mermap_cpu_end(int cpu)
{
	return MERMAP_BASE_ADDR + ((cpu + 1) * MERMAP_CPU_REGION_SIZE);

}

static inline void mermap_flush_tlb(int cpu, struct mermap_cpu *mc)
{
#ifdef CONFIG_MERMAP_KUNIT_TEST
	mc->tlb_flushes++;
#endif
	arch_mermap_flush_tlb();
}

/* Call with migration disabled. */
static inline struct mermap_alloc *mermap_alloc(struct mm_struct *mm,
						unsigned long size, bool use_reserve)
{
	int cpu = raw_smp_processor_id();
	struct mermap_cpu *mc = this_cpu_ptr(mm->mermap.cpu);
	unsigned long cpu_end = mermap_cpu_end(cpu);
	struct mermap_alloc *alloc = NULL;

	/*
	 * This is an extremely stupid allocator, there can only ever be a small
	 * number of allocations so everything just works on linear search.
	 *
	 * Allocations are "in order", i.e. if the whole region is free it
	 * allocates from the beginning. If there are any existing allocations
	 * it allocates from right after the last (highest address) one. Any
	 * free space before that goes unused.
	 *
	 * Once an allocation has been freed, the space it occupied must be flushed
	 * from the TLB before it can be reused.
	 *
	 * Visual example of how this is suppose to behave (A for allocated, T for
	 * TLB-flush-pending):
	 *
	 *  _______________ Start with everything free.
	 *  AaaA___________ Allocate something.
	 *  TttT___________ Free it. (Region needs a TLB flush now).
	 *  TttTAaaaaaaaA__ Allocate something else.
	 *  TttTAaaaaaaaAAA Allocate the remaining space.
	 *  TttTTtttttttTAA Free the allocation before last.
	 *  ^^^^^^^^^^^^^   This could all be reused now but for simplicity it
	 *                  isn't. Another allocation at this point  will fail.
	 *  TttTTtttttttTTT Free the last allocation.
	 *  _______________ Next time we allocate, first flush the TLB
	 *  AA_____________ Now we're back at the beginning.
	 */

	if (use_reserve) {
		if (WARN_ON_ONCE(size != PAGE_SIZE))
			return NULL;
		lockdep_assert_preemption_disabled();
	} else {
		cpu_end -= PAGE_SIZE;
	}

	if (WARN_ON_ONCE(!in_task()))
		return NULL;
	guard(preempt)();

	/* Out of already-available space? */
	if (mc->next_addr + size > cpu_end) {
		unsigned long new_next = mermap_cpu_base(cpu);

		/* Would we have space after a TLB flush? */
		for (int i = 0; i < ARRAY_SIZE(mc->allocs); i++) {
			struct mermap_alloc *alloc = &mc->allocs[i];

			/*
			 * The space between the uppermost allocated alloc->end
			 * (or the base of the CPU's region if there are no
			 * current allocations) and mc->next_addr has been
			 * unmapped in the pagetables, but not flushed from the
			 * TLB. Set new_next to point to the beginning of that
			 * space.
			 */
			if (READ_ONCE(alloc->in_use))
				new_next = max(new_next, alloc->end);
		}
		if (size > cpu_end - new_next)
			return NULL;

		mermap_flush_tlb(cpu, mc);
		mc->next_addr = new_next;
	}

	/* Find an alloc-tracking structure to use */
	for (int i = 0; i < ARRAY_SIZE(mc->allocs); i++) {
		if (!READ_ONCE(mc->allocs[i].in_use)) {
			alloc = &mc->allocs[i];
			break;
		}
	}
	if (!alloc)
		return NULL;
	alloc->in_use = true;
	alloc->base = mc->next_addr;
	alloc->end = alloc->base + size;
	mc->next_addr += size;

	return alloc;
}

struct set_pte_ctx {
	pgprot_t prot;
	unsigned long next_pfn;
};

static inline int do_set_pte(pte_t *pte, unsigned long addr, void *data)
{
	struct set_pte_ctx *ctx = data;

	set_pte(pte, pfn_pte(ctx->next_pfn, ctx->prot));
	ctx->next_pfn++;

	return 0;
}

static struct mermap_alloc *
__mermap_get(struct mm_struct *mm, struct page *page,
	     unsigned long size, pgprot_t prot, bool use_reserve)
{
	struct mermap_alloc *alloc = NULL;
	struct set_pte_ctx ctx;
	int err;

	if (size > MERMAP_CPU_REGION_SIZE || WARN_ON_ONCE(!mm || !mm->mermap.cpu))
		return NULL;
	if (WARN_ON_ONCE(!arch_mermap_pgprot_allowed(prot)))
		return NULL;

	size = PAGE_ALIGN(size);

	migrate_disable();

	alloc = mermap_alloc(mm, size, use_reserve);
	if (!alloc) {
		migrate_enable();
		return NULL;
	}

	/* This probably wants to be optimised. */
	ctx.prot = prot;
	ctx.next_pfn = page_to_pfn(page);
	err = apply_to_existing_page_range(mm, alloc->base, size, do_set_pte, &ctx);
	if (err) {
		WRITE_ONCE(alloc->in_use, false);
		return NULL;
	}

	return alloc;
}

/*
 * Allocate a region of virtual memory, and map the page into it. This tries
 * pretty hard to be fast but doesn't try very hard at all to actually succeed.
 *
 * The returned region is physically local to the current mm. It is _logically_
 * local to the current CPU but this is not enforced by hardware so it can be
 * exploited to mitigate CPU vulns. This means the caller must not map memory
 * here that doesn't belong to the current process. The caller must also perform
 * a full TLB flush of the region before freeing the pages that have been mapped
 * here.
 *
 * This may only be called from process context, and the caller must arrange to
 * first call mermap_mm_prepare(). (It would be possible to support this in IRQ,
 * but it seems unlikely there's a valid usecase given the TLB flushing
 * requirements). If it succeeds, it disables migration until you call
 * mermap_put().
 *
 * This is guaranteed not to allocate.
 *
 * Use mermap_addr() to get the actual address of the mapped region.
 */
struct mermap_alloc *mermap_get(struct page *page, unsigned long size, pgprot_t prot)
{
	return __mermap_get(current->mm, page, size, prot, false);
}
EXPORT_SYMBOL(mermap_get);

/*
 * Allocate a single PAGE_SIZE page via mermap_get(), requiring preemption to be
 * off until it is freed. This always succeeds.
 */
void *mermap_get_reserved(struct page *page, pgprot_t prot)
{
	lockdep_assert_preemption_disabled();
	return __mermap_get(current->mm, page, PAGE_SIZE, prot, true);
}
EXPORT_SYMBOL(mermap_get_reserved);

/*
 * Internal - do unconditional (cheap) setup that's done for every mm. This
 * doesn't actually prepare the mermap for use until someone calls
 * mermap_mm_prepare().
 */
void mermap_mm_init(struct mm_struct *mm)
{
	mutex_init(&mm->mermap.init_lock);
}

/*
 * Set up the mermap for this mm. The caller doesn't need to call
 * mermap_mm_teardown(), that's take care of by the normal mm teardown
 * mechanism. This is idempotent and thread-safe.
 */
int mermap_mm_prepare(struct mm_struct *mm)
{
	int err = 0;
	int cpu;

	guard(mutex)(&mm->mermap.init_lock);

	/* Already done? */
	if (likely(mm->mermap.cpu))
		return 0;

	mm->mermap.cpu = alloc_percpu_gfp(struct mermap_cpu,
					  GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!mm->mermap.cpu)
		return -ENOMEM;

	/* So we can use this from the page allocator, preallocate pagetables. */
	mm_flags_set(MMF_LOCAL_REGION_USED, mm);
	for_each_possible_cpu(cpu) {
		unsigned long base = mermap_cpu_base(cpu);

		err = apply_to_page_range(mm, base, MERMAP_CPU_REGION_SIZE,
					  set_unmapped_pte, NULL);
		if (err) {
			/*
			 * Clear .cpu now to inform mermap_ready(). Any partial
			 * page tables get cleared up by mm teardown.
			 */
			free_percpu(mm->mermap.cpu);
			mm->mermap.cpu = NULL;
			break;
		}
		per_cpu_ptr(mm->mermap.cpu, cpu)->next_addr = base;
	}

	return err;
}
EXPORT_SYMBOL_GPL(mermap_mm_prepare);

/* Clean up mermap stuff on mm teardown. */
void mermap_mm_teardown(struct mm_struct *mm)
{
	int cpu;

	if (!mm->mermap.cpu)
		return;

	for_each_possible_cpu(cpu) {
		struct mermap_cpu *mc = this_cpu_ptr(mm->mermap.cpu);

		for (int i = 0; i < ARRAY_SIZE(mc->allocs); i++)
			WARN_ON_ONCE(mc->allocs[i].in_use);
	}

	free_percpu(mm->mermap.cpu);
}
