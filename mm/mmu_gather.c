#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mmdebug.h>
#include <linux/mm_types.h>
#include <linux/mm_inline.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/smp.h>
#include <linux/swap.h>
#include <linux/rmap.h>
#include <linux/oom.h>

#include <asm/pgalloc.h>
#include <asm/tlb.h>

#ifndef CONFIG_MMU_GATHER_NO_GATHER
/*
 * The swp_entry asynchronous release mechanism for multiple processes exiting
 * simultaneously.
 *
 * During the multiple exiting processes releasing their own mm simultaneously,
 * the swap entries in the exiting processes are handled by isolating, caching
 * and handing over to an asynchronous kworker to complete the release.
 *
 * The conditions for the exiting process entering the swp_entry asynchronous
 * release path:
 * 1. The exiting process's MM_SWAPENTS count is >= SWAP_CLUSTER_MAX, avoiding
 *    to alloc struct mmu_swap_gather frequently.
 * 2. The number of exiting processes is >= NR_MIN_EXITING_PROCESSES.
 *
 * Since the time for determining the number of exiting processes is dynamic,
 * the exiting process may start to enter the swp_entry asynchronous release
 * at the beginning or middle stage of the exiting process's swp_entry release
 * path.
 *
 * Once an exiting process enters the swp_entry asynchronous release, all remaining
 * swap entries in this exiting process need to be fully released by asynchronous
 * kworker theoretically.
 *
 * The function of the swp_entry asynchronous release:
 * 1. Alleviate the high system cpu load caused by multiple exiting processes
 *    running simultaneously.
 * 2. Reduce lock competition in swap entry free path by an asynchronous kworker
 *    instead of multiple exiting processes parallel execution.
 * 3. Release memory occupied by exiting processes more efficiently.
 */

/*
 * The min number of exiting processes required for swp_entry asynchronous release
 */
#define NR_MIN_EXITING_PROCESSES 2

atomic_t nr_exiting_processes = ATOMIC_INIT(0);
static struct kmem_cache *swap_gather_cachep;
static struct workqueue_struct *swapfree_wq;
static DEFINE_STATIC_KEY_TRUE(tlb_swap_asyncfree_disabled);

static int __init tlb_swap_async_free_setup(void)
{
	swapfree_wq = alloc_workqueue("smfree_wq", WQ_UNBOUND |
		WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
	if (!swapfree_wq)
		goto fail;

	swap_gather_cachep = kmem_cache_create("swap_gather",
		sizeof(struct mmu_swap_gather),
		0, SLAB_TYPESAFE_BY_RCU | SLAB_PANIC | SLAB_ACCOUNT,
		NULL);
	if (!swap_gather_cachep)
		goto kcache_fail;

	static_branch_disable(&tlb_swap_asyncfree_disabled);
	return 0;

kcache_fail:
	destroy_workqueue(swapfree_wq);
fail:
	return -ENOMEM;
}
postcore_initcall(tlb_swap_async_free_setup);

static void __tlb_swap_gather_free(struct mmu_swap_gather *swap_gather)
{
	struct mmu_swap_batch *swap_batch, *next;

	for (swap_batch = swap_gather->local.next; swap_batch; swap_batch = next) {
		next = swap_batch->next;
		free_page((unsigned long)swap_batch);
	}
	swap_gather->local.next = NULL;
	kmem_cache_free(swap_gather_cachep, swap_gather);
}

static void tlb_swap_async_free_work(struct work_struct *w)
{
	int i, nr_multi, nr_free;
	swp_entry_t start_entry;
	struct mmu_swap_batch *swap_batch;
	struct mmu_swap_gather *swap_gather = container_of(w,
		struct mmu_swap_gather, free_work);

	/* Release swap entries cached in mmu_swap_batch. */
	for (swap_batch = &swap_gather->local; swap_batch && swap_batch->nr;
	    swap_batch = swap_batch->next) {
		nr_free = 0;
		for (i = 0; i < swap_batch->nr; i++) {
			if (unlikely(encoded_swpentry_flags(swap_batch->encoded_entrys[i]) &
			    ENCODED_SWPENTRY_BIT_NR_ENTRYS_NEXT)) {
				start_entry = encoded_swpentry_data(swap_batch->encoded_entrys[i]);
				nr_multi = encoded_nr_swpentrys(swap_batch->encoded_entrys[++i]);
				free_swap_and_cache_nr(start_entry, nr_multi);
				nr_free += 2;
			} else {
				start_entry = encoded_swpentry_data(swap_batch->encoded_entrys[i]);
				free_swap_and_cache_nr(start_entry, 1);
				nr_free++;
			}
		}
		swap_batch->nr -= nr_free;
		WARN_ON_ONCE(swap_batch->nr);
	}
	__tlb_swap_gather_free(swap_gather);
}

static bool __tlb_swap_gather_mmu_check(struct mmu_gather *tlb)
{
	/*
	 * Only the exiting processes with the MM_SWAPENTS counter >=
	 * SWAP_CLUSTER_MAX have the opportunity to release their swap
	 * entries by asynchronous kworker.
	 */
	if (!task_is_dying() ||
	    get_mm_counter(tlb->mm, MM_SWAPENTS) < SWAP_CLUSTER_MAX)
		return true;

	atomic_inc(&nr_exiting_processes);
	if (atomic_read(&nr_exiting_processes) < NR_MIN_EXITING_PROCESSES)
		tlb->swp_freeable = 1;
	else
		tlb->swp_freeing = 1;

	return false;
}

/**
 * __tlb_swap_gather_init - Initialize an mmu_swap_gather structure
 * for swp_entry tear-down.
 * @tlb: the mmu_swap_gather structure belongs to tlb
 */
static bool __tlb_swap_gather_init(struct mmu_gather *tlb)
{
	tlb->swp = kmem_cache_alloc(swap_gather_cachep, GFP_ATOMIC | GFP_NOWAIT);
	if (unlikely(!tlb->swp))
		return false;

	tlb->swp->local.next  = NULL;
	tlb->swp->local.nr    = 0;
	tlb->swp->local.max   = ARRAY_SIZE(tlb->swp->__encoded_entrys);

	tlb->swp->active      = &tlb->swp->local;
	tlb->swp->batch_count = 0;

	INIT_WORK(&tlb->swp->free_work, tlb_swap_async_free_work);
	return true;
}

static void __tlb_swap_gather_mmu(struct mmu_gather *tlb)
{
	if (static_branch_unlikely(&tlb_swap_asyncfree_disabled))
		return;

	tlb->swp = NULL;
	tlb->swp_freeable = 0;
	tlb->swp_freeing = 0;
	tlb->swp_disable = 0;

	if (__tlb_swap_gather_mmu_check(tlb))
		return;

	/*
	 * If the exiting process meets the conditions of
	 * swp_entry asynchronous release, an mmu_swap_gather
	 * structure will be initialized.
	 */
	if (tlb->swp_freeing)
		__tlb_swap_gather_init(tlb);
}

static void __tlb_swap_gather_queuework(struct mmu_gather *tlb, bool finish)
{
	queue_work(swapfree_wq, &tlb->swp->free_work);
	tlb->swp = NULL;
	if (!finish)
		__tlb_swap_gather_init(tlb);
}

static bool __tlb_swap_next_batch(struct mmu_gather *tlb)
{
	struct mmu_swap_batch *swap_batch;

	if (tlb->swp->batch_count == MAX_SWAP_GATHER_BATCH_COUNT)
		goto free;

	swap_batch = (void *)__get_free_page(GFP_ATOMIC | GFP_NOWAIT);
	if (unlikely(!swap_batch))
		goto free;

	swap_batch->next = NULL;
	swap_batch->nr   = 0;
	swap_batch->max  = MAX_SWAP_GATHER_BATCH;

	tlb->swp->active->next = swap_batch;
	tlb->swp->active = swap_batch;
	tlb->swp->batch_count++;
	return true;
free:
	/* batch move to wq */
	__tlb_swap_gather_queuework(tlb, false);
	return false;
}

/**
 * __tlb_remove_swap_entries - the swap entries in exiting process are
 * isolated, batch cached in struct mmu_swap_batch.
 * @tlb: the current mmu_gather
 * @entry: swp_entry to be isolated and cached
 * @nr: the number of consecutive entries starting from entry parameter.
 */
bool __tlb_remove_swap_entries(struct mmu_gather *tlb,
			     swp_entry_t entry, int nr)
{
	struct mmu_swap_batch *swap_batch;
	unsigned long flags = 0;
	bool ret = false;

	if (tlb->swp_disable)
		return ret;

	if (!tlb->swp_freeable && !tlb->swp_freeing)
		return ret;


	if (tlb->swp_freeable) {
		if (atomic_read(&nr_exiting_processes) <
		    NR_MIN_EXITING_PROCESSES)
			return ret;
		/*
		 * If the current number of exiting processes
		 * is >= NR_MIN_EXITING_PROCESSES, the exiting
		 * process with swp_freeable state will enter
		 * swp_freeing state to start releasing its
		 * remaining swap entries by the asynchronous
		 * kworker.
		 */
		tlb->swp_freeable = 0;
		tlb->swp_freeing = 1;
	}

	VM_BUG_ON(tlb->swp_freeable || !tlb->swp_freeing);
	if (!tlb->swp && !__tlb_swap_gather_init(tlb))
		return ret;

	swap_batch = tlb->swp->active;
	if (unlikely(swap_batch->nr >= swap_batch->max - 1)) {
		__tlb_swap_gather_queuework(tlb, false);
		return ret;
	}

	if (likely(nr == 1)) {
		swap_batch->encoded_entrys[swap_batch->nr++] = encode_swpentry(entry, flags);
	} else {
		flags |= ENCODED_SWPENTRY_BIT_NR_ENTRYS_NEXT;
		swap_batch->encoded_entrys[swap_batch->nr++] = encode_swpentry(entry, flags);
		swap_batch->encoded_entrys[swap_batch->nr++] = encode_nr_swpentrys(nr);
	}
	ret = true;

	if (swap_batch->nr >= swap_batch->max - 1) {
		if (!__tlb_swap_next_batch(tlb))
			goto exit;
		swap_batch = tlb->swp->active;
	}
	VM_BUG_ON(swap_batch->nr > swap_batch->max - 1);
exit:
	return ret;
}

static void __tlb_batch_swap_finish(struct mmu_gather *tlb)
{
	if (tlb->swp_disable)
		return;

	if (!tlb->swp_freeable && !tlb->swp_freeing)
		return;

	if (tlb->swp_freeable) {
		tlb->swp_freeable = 0;
		VM_BUG_ON(tlb->swp_freeing);
		goto exit;
	}
	tlb->swp_freeing = 0;
	if (unlikely(!tlb->swp))
		goto exit;

	__tlb_swap_gather_queuework(tlb, true);
exit:
	atomic_dec(&nr_exiting_processes);
}

static bool tlb_next_batch(struct mmu_gather *tlb)
{
	struct mmu_gather_batch *batch;

	/* Limit batching if we have delayed rmaps pending */
	if (tlb->delayed_rmap && tlb->active != &tlb->local)
		return false;

	batch = tlb->active;
	if (batch->next) {
		tlb->active = batch->next;
		return true;
	}

	if (tlb->batch_count == MAX_GATHER_BATCH_COUNT)
		return false;

	batch = (void *)__get_free_page(GFP_NOWAIT | __GFP_NOWARN);
	if (!batch)
		return false;

	tlb->batch_count++;
	batch->next = NULL;
	batch->nr   = 0;
	batch->max  = MAX_GATHER_BATCH;

	tlb->active->next = batch;
	tlb->active = batch;

	return true;
}

#ifdef CONFIG_SMP
static void tlb_flush_rmap_batch(struct mmu_gather_batch *batch, struct vm_area_struct *vma)
{
	struct encoded_page **pages = batch->encoded_pages;

	for (int i = 0; i < batch->nr; i++) {
		struct encoded_page *enc = pages[i];

		if (encoded_page_flags(enc) & ENCODED_PAGE_BIT_DELAY_RMAP) {
			struct page *page = encoded_page_ptr(enc);
			unsigned int nr_pages = 1;

			if (unlikely(encoded_page_flags(enc) &
				     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
				nr_pages = encoded_nr_pages(pages[++i]);

			folio_remove_rmap_ptes(page_folio(page), page, nr_pages,
					       vma);
		}
	}
}

/**
 * tlb_flush_rmaps - do pending rmap removals after we have flushed the TLB
 * @tlb: the current mmu_gather
 * @vma: The memory area from which the pages are being removed.
 *
 * Note that because of how tlb_next_batch() above works, we will
 * never start multiple new batches with pending delayed rmaps, so
 * we only need to walk through the current active batch and the
 * original local one.
 */
void tlb_flush_rmaps(struct mmu_gather *tlb, struct vm_area_struct *vma)
{
	if (!tlb->delayed_rmap)
		return;

	tlb_flush_rmap_batch(&tlb->local, vma);
	if (tlb->active != &tlb->local)
		tlb_flush_rmap_batch(tlb->active, vma);
	tlb->delayed_rmap = 0;
}
#endif

/*
 * We might end up freeing a lot of pages. Reschedule on a regular
 * basis to avoid soft lockups in configurations without full
 * preemption enabled. The magic number of 512 folios seems to work.
 */
#define MAX_NR_FOLIOS_PER_FREE		512

static void __tlb_batch_free_encoded_pages(struct mmu_gather_batch *batch)
{
	struct encoded_page **pages = batch->encoded_pages;
	unsigned int nr, nr_pages;

	while (batch->nr) {
		if (!page_poisoning_enabled_static() && !want_init_on_free()) {
			nr = min(MAX_NR_FOLIOS_PER_FREE, batch->nr);

			/*
			 * Make sure we cover page + nr_pages, and don't leave
			 * nr_pages behind when capping the number of entries.
			 */
			if (unlikely(encoded_page_flags(pages[nr - 1]) &
				     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
				nr++;
		} else {
			/*
			 * With page poisoning and init_on_free, the time it
			 * takes to free memory grows proportionally with the
			 * actual memory size. Therefore, limit based on the
			 * actual memory size and not the number of involved
			 * folios.
			 */
			for (nr = 0, nr_pages = 0;
			     nr < batch->nr && nr_pages < MAX_NR_FOLIOS_PER_FREE;
			     nr++) {
				if (unlikely(encoded_page_flags(pages[nr]) &
					     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
					nr_pages += encoded_nr_pages(pages[++nr]);
				else
					nr_pages++;
			}
		}

		free_pages_and_swap_cache(pages, nr);
		pages += nr;
		batch->nr -= nr;

		cond_resched();
	}
}

static void tlb_batch_pages_flush(struct mmu_gather *tlb)
{
	struct mmu_gather_batch *batch;

	for (batch = &tlb->local; batch && batch->nr; batch = batch->next)
		__tlb_batch_free_encoded_pages(batch);
	tlb->active = &tlb->local;
}

static void tlb_batch_list_free(struct mmu_gather *tlb)
{
	struct mmu_gather_batch *batch, *next;

	for (batch = tlb->local.next; batch; batch = next) {
		next = batch->next;
		free_pages((unsigned long)batch, 0);
	}
	tlb->local.next = NULL;
}

static bool __tlb_remove_folio_pages_size(struct mmu_gather *tlb,
		struct page *page, unsigned int nr_pages, bool delay_rmap,
		int page_size)
{
	int flags = delay_rmap ? ENCODED_PAGE_BIT_DELAY_RMAP : 0;
	struct mmu_gather_batch *batch;

	VM_BUG_ON(!tlb->end);

#ifdef CONFIG_MMU_GATHER_PAGE_SIZE
	VM_WARN_ON(tlb->page_size != page_size);
	VM_WARN_ON_ONCE(nr_pages != 1 && page_size != PAGE_SIZE);
	VM_WARN_ON_ONCE(page_folio(page) != page_folio(page + nr_pages - 1));
#endif

	batch = tlb->active;
	/*
	 * Add the page and check if we are full. If so
	 * force a flush.
	 */
	if (likely(nr_pages == 1)) {
		batch->encoded_pages[batch->nr++] = encode_page(page, flags);
	} else {
		flags |= ENCODED_PAGE_BIT_NR_PAGES_NEXT;
		batch->encoded_pages[batch->nr++] = encode_page(page, flags);
		batch->encoded_pages[batch->nr++] = encode_nr_pages(nr_pages);
	}
	/*
	 * Make sure that we can always add another "page" + "nr_pages",
	 * requiring two entries instead of only a single one.
	 */
	if (batch->nr >= batch->max - 1) {
		if (!tlb_next_batch(tlb))
			return true;
		batch = tlb->active;
	}
	VM_BUG_ON_PAGE(batch->nr > batch->max - 1, page);

	return false;
}

bool __tlb_remove_folio_pages(struct mmu_gather *tlb, struct page *page,
		unsigned int nr_pages, bool delay_rmap)
{
	return __tlb_remove_folio_pages_size(tlb, page, nr_pages, delay_rmap,
					     PAGE_SIZE);
}

bool __tlb_remove_page_size(struct mmu_gather *tlb, struct page *page,
		bool delay_rmap, int page_size)
{
	return __tlb_remove_folio_pages_size(tlb, page, 1, delay_rmap, page_size);
}

#endif /* MMU_GATHER_NO_GATHER */

#ifdef CONFIG_MMU_GATHER_TABLE_FREE

static void __tlb_remove_table_free(struct mmu_table_batch *batch)
{
	int i;

	for (i = 0; i < batch->nr; i++)
		__tlb_remove_table(batch->tables[i]);

	free_page((unsigned long)batch);
}

#ifdef CONFIG_MMU_GATHER_RCU_TABLE_FREE

/*
 * Semi RCU freeing of the page directories.
 *
 * This is needed by some architectures to implement software pagetable walkers.
 *
 * gup_fast() and other software pagetable walkers do a lockless page-table
 * walk and therefore needs some synchronization with the freeing of the page
 * directories. The chosen means to accomplish that is by disabling IRQs over
 * the walk.
 *
 * Architectures that use IPIs to flush TLBs will then automagically DTRT,
 * since we unlink the page, flush TLBs, free the page. Since the disabling of
 * IRQs delays the completion of the TLB flush we can never observe an already
 * freed page.
 *
 * Architectures that do not have this (PPC) need to delay the freeing by some
 * other means, this is that means.
 *
 * What we do is batch the freed directory pages (tables) and RCU free them.
 * We use the sched RCU variant, as that guarantees that IRQ/preempt disabling
 * holds off grace periods.
 *
 * However, in order to batch these pages we need to allocate storage, this
 * allocation is deep inside the MM code and can thus easily fail on memory
 * pressure. To guarantee progress we fall back to single table freeing, see
 * the implementation of tlb_remove_table_one().
 *
 */

static void tlb_remove_table_smp_sync(void *arg)
{
	/* Simply deliver the interrupt */
}

void tlb_remove_table_sync_one(void)
{
	/*
	 * This isn't an RCU grace period and hence the page-tables cannot be
	 * assumed to be actually RCU-freed.
	 *
	 * It is however sufficient for software page-table walkers that rely on
	 * IRQ disabling.
	 */
	smp_call_function(tlb_remove_table_smp_sync, NULL, 1);
}

static void tlb_remove_table_rcu(struct rcu_head *head)
{
	__tlb_remove_table_free(container_of(head, struct mmu_table_batch, rcu));
}

static void tlb_remove_table_free(struct mmu_table_batch *batch)
{
	call_rcu(&batch->rcu, tlb_remove_table_rcu);
}

#else /* !CONFIG_MMU_GATHER_RCU_TABLE_FREE */

static void tlb_remove_table_free(struct mmu_table_batch *batch)
{
	__tlb_remove_table_free(batch);
}

#endif /* CONFIG_MMU_GATHER_RCU_TABLE_FREE */

/*
 * If we want tlb_remove_table() to imply TLB invalidates.
 */
static inline void tlb_table_invalidate(struct mmu_gather *tlb)
{
	if (tlb_needs_table_invalidate()) {
		/*
		 * Invalidate page-table caches used by hardware walkers. Then
		 * we still need to RCU-sched wait while freeing the pages
		 * because software walkers can still be in-flight.
		 */
		tlb_flush_mmu_tlbonly(tlb);
	}
}

static void tlb_remove_table_one(void *table)
{
	tlb_remove_table_sync_one();
	__tlb_remove_table(table);
}

static void tlb_table_flush(struct mmu_gather *tlb)
{
	struct mmu_table_batch **batch = &tlb->batch;

	if (*batch) {
		tlb_table_invalidate(tlb);
		tlb_remove_table_free(*batch);
		*batch = NULL;
	}
}

void tlb_remove_table(struct mmu_gather *tlb, void *table)
{
	struct mmu_table_batch **batch = &tlb->batch;

	if (*batch == NULL) {
		*batch = (struct mmu_table_batch *)__get_free_page(GFP_NOWAIT | __GFP_NOWARN);
		if (*batch == NULL) {
			tlb_table_invalidate(tlb);
			tlb_remove_table_one(table);
			return;
		}
		(*batch)->nr = 0;
	}

	(*batch)->tables[(*batch)->nr++] = table;
	if ((*batch)->nr == MAX_TABLE_BATCH)
		tlb_table_flush(tlb);
}

static inline void tlb_table_init(struct mmu_gather *tlb)
{
	tlb->batch = NULL;
}

#else /* !CONFIG_MMU_GATHER_TABLE_FREE */

static inline void tlb_table_flush(struct mmu_gather *tlb) { }
static inline void tlb_table_init(struct mmu_gather *tlb) { }

#endif /* CONFIG_MMU_GATHER_TABLE_FREE */

static void tlb_flush_mmu_free(struct mmu_gather *tlb)
{
	tlb_table_flush(tlb);
#ifndef CONFIG_MMU_GATHER_NO_GATHER
	tlb_batch_pages_flush(tlb);
#endif
}

void tlb_flush_mmu(struct mmu_gather *tlb)
{
	tlb_flush_mmu_tlbonly(tlb);
	tlb_flush_mmu_free(tlb);
}

static void __tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm,
			     bool fullmm)
{
	tlb->mm = mm;
	tlb->fullmm = fullmm;

#ifndef CONFIG_MMU_GATHER_NO_GATHER
	tlb->need_flush_all = 0;
	tlb->local.next = NULL;
	tlb->local.nr   = 0;
	tlb->local.max  = ARRAY_SIZE(tlb->__pages);
	tlb->active     = &tlb->local;
	tlb->batch_count = 0;

	tlb->swp_disable = 1;
	__tlb_swap_gather_mmu(tlb);
#endif
	tlb->delayed_rmap = 0;

	tlb_table_init(tlb);
#ifdef CONFIG_MMU_GATHER_PAGE_SIZE
	tlb->page_size = 0;
#endif

	__tlb_reset_range(tlb);
	inc_tlb_flush_pending(tlb->mm);
}

/**
 * tlb_gather_mmu - initialize an mmu_gather structure for page-table tear-down
 * @tlb: the mmu_gather structure to initialize
 * @mm: the mm_struct of the target address space
 *
 * Called to initialize an (on-stack) mmu_gather structure for page-table
 * tear-down from @mm.
 */
void tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm)
{
	__tlb_gather_mmu(tlb, mm, false);
}

/**
 * tlb_gather_mmu_fullmm - initialize an mmu_gather structure for page-table tear-down
 * @tlb: the mmu_gather structure to initialize
 * @mm: the mm_struct of the target address space
 *
 * In this case, @mm is without users and we're going to destroy the
 * full address space (exit/execve).
 *
 * Called to initialize an (on-stack) mmu_gather structure for page-table
 * tear-down from @mm.
 */
void tlb_gather_mmu_fullmm(struct mmu_gather *tlb, struct mm_struct *mm)
{
	__tlb_gather_mmu(tlb, mm, true);
}

/**
 * tlb_finish_mmu - finish an mmu_gather structure
 * @tlb: the mmu_gather structure to finish
 *
 * Called at the end of the shootdown operation to free up any resources that
 * were required.
 */
void tlb_finish_mmu(struct mmu_gather *tlb)
{
	/*
	 * If there are parallel threads are doing PTE changes on same range
	 * under non-exclusive lock (e.g., mmap_lock read-side) but defer TLB
	 * flush by batching, one thread may end up seeing inconsistent PTEs
	 * and result in having stale TLB entries.  So flush TLB forcefully
	 * if we detect parallel PTE batching threads.
	 *
	 * However, some syscalls, e.g. munmap(), may free page tables, this
	 * needs force flush everything in the given range. Otherwise this
	 * may result in having stale TLB entries for some architectures,
	 * e.g. aarch64, that could specify flush what level TLB.
	 */
	if (mm_tlb_flush_nested(tlb->mm)) {
		/*
		 * The aarch64 yields better performance with fullmm by
		 * avoiding multiple CPUs spamming TLBI messages at the
		 * same time.
		 *
		 * On x86 non-fullmm doesn't yield significant difference
		 * against fullmm.
		 */
		tlb->fullmm = 1;
		__tlb_reset_range(tlb);
		tlb->freed_tables = 1;
	}

	tlb_flush_mmu(tlb);

#ifndef CONFIG_MMU_GATHER_NO_GATHER
	tlb_batch_list_free(tlb);
	__tlb_batch_swap_finish(tlb);
#endif
	dec_tlb_flush_pending(tlb->mm);
}
