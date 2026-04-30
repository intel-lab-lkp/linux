// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 *  Manages the free list, the system allocates free pages here.
 *  Note that kmalloc() lives in slab.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  Reshaped it to be a zoned allocator, Ingo Molnar, Red Hat, 1999
 *  Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 *  Zone balancing, Kanoj Sarcar, SGI, Jan 2000
 *  Per cpu hot/cold page lists, bulk allocation, Martin J. Bligh, Sept 2002
 *          (lots of bits borrowed from Ingo Molnar & Andrew Morton)
 */

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/kasan.h>
#include <linux/kmsan.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/ratelimit.h>
#include <linux/oom.h>
#include <linux/topology.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/pagevec.h>
#include <linux/memory_hotplug.h>
#include <linux/nodemask.h>
#include <linux/vmstat.h>
#include <linux/fault-inject.h>
#include <linux/compaction.h>
#include <trace/events/kmem.h>
#include <trace/events/oom.h>
#include <linux/prefetch.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/sched/mm.h>
#include <linux/page_owner.h>
#include <linux/page_table_check.h>
#include <linux/memcontrol.h>
#include <linux/ftrace.h>
#include <linux/lockdep.h>
#include <linux/psi.h>
#include <linux/khugepaged.h>
#include <linux/workqueue.h>
#include <linux/delayacct.h>
#include <linux/cacheinfo.h>
#include <linux/pgalloc_tag.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/div64.h>
#include "internal.h"
#include "shuffle.h"
#include "page_reporting.h"

/* Free Page Internal flags: for internal, non-pcp variants of free_pages(). */
typedef int __bitwise fpi_t;

/* No special request */
#define FPI_NONE		((__force fpi_t)0)

/*
 * Skip free page reporting notification for the (possibly merged) page.
 * This does not hinder free page reporting from grabbing the page,
 * reporting it and marking it "reported" -  it only skips notifying
 * the free page reporting infrastructure about a newly freed page. For
 * example, used when temporarily pulling a page from a freelist and
 * putting it back unmodified.
 */
#define FPI_SKIP_REPORT_NOTIFY	((__force fpi_t)BIT(0))

/*
 * Place the (possibly merged) page to the tail of the freelist. Will ignore
 * page shuffling (relevant code - e.g., memory onlining - is expected to
 * shuffle the whole zone).
 *
 * Note: No code should rely on this flag for correctness - it's purely
 *       to allow for optimizations when handing back either fresh pages
 *       (memory onlining) or untouched pages (page isolation, free page
 *       reporting).
 */
#define FPI_TO_TAIL		((__force fpi_t)BIT(1))

/* Free the page without taking locks. Rely on trylock only. */
#define FPI_TRYLOCK		((__force fpi_t)BIT(2))

/* prevent >1 _updater_ of zone percpu pageset ->high and ->batch fields */
static DEFINE_MUTEX(pcp_batch_high_lock);
#define MIN_PERCPU_PAGELIST_HIGH_FRACTION (8)

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
/*
 * On SMP, spin_trylock is sufficient protection.
 * On PREEMPT_RT, spin_trylock is equivalent on both SMP and UP.
 * Pass flags to a no-op inline function to typecheck and silence the unused
 * variable warning.
 */
static inline void __pcp_trylock_noop(unsigned long *flags) { }
#define pcp_trylock_prepare(flags)	__pcp_trylock_noop(&(flags))
#define pcp_trylock_finish(flags)	__pcp_trylock_noop(&(flags))
#else

/* UP spin_trylock always succeeds so disable IRQs to prevent re-entrancy. */
#define pcp_trylock_prepare(flags)	local_irq_save(flags)
#define pcp_trylock_finish(flags)	local_irq_restore(flags)
#endif

/*
 * Locking a pcp requires a PCP lookup followed by a spinlock. To avoid
 * a migration causing the wrong PCP to be locked and remote memory being
 * potentially allocated, pin the task to the CPU for the lookup+lock.
 * preempt_disable is used on !RT because it is faster than migrate_disable.
 * migrate_disable is used on RT because otherwise RT spinlock usage is
 * interfered with and a high priority task cannot preempt the allocator.
 */
#ifndef CONFIG_PREEMPT_RT
#define pcpu_task_pin()		preempt_disable()
#define pcpu_task_unpin()	preempt_enable()
#else
#define pcpu_task_pin()		migrate_disable()
#define pcpu_task_unpin()	migrate_enable()
#endif

/*
 * Generic helper to lookup and a per-cpu variable with an embedded spinlock.
 * Return value should be used with equivalent unlock helper.
 */
#define pcpu_spin_trylock(type, member, ptr)				\
({									\
	type *_ret;							\
	pcpu_task_pin();						\
	_ret = this_cpu_ptr(ptr);					\
	if (!spin_trylock(&_ret->member)) {				\
		pcpu_task_unpin();					\
		_ret = NULL;						\
	}								\
	_ret;								\
})

#define pcpu_spin_unlock(member, ptr)					\
({									\
	spin_unlock(&ptr->member);					\
	pcpu_task_unpin();						\
})

/* struct per_cpu_pages specific helpers. */
#define pcp_spin_trylock(ptr, UP_flags)					\
({									\
	struct per_cpu_pages *__ret;					\
	pcp_trylock_prepare(UP_flags);					\
	__ret = pcpu_spin_trylock(struct per_cpu_pages, lock, ptr);	\
	if (!__ret)							\
		pcp_trylock_finish(UP_flags);				\
	__ret;								\
})

#define pcp_spin_unlock(ptr, UP_flags)					\
({									\
	pcpu_spin_unlock(lock, ptr);					\
	pcp_trylock_finish(UP_flags);					\
})

/*
 * With the UP spinlock implementation, when we spin_lock(&pcp->lock) (for i.e.
 * a potentially remote cpu drain) and get interrupted by an operation that
 * attempts pcp_spin_trylock(), we can't rely on the trylock failure due to UP
 * spinlock assumptions making the trylock a no-op. So we have to turn that
 * spin_lock() to a spin_lock_irqsave(). This works because on UP there are no
 * remote cpu's so we can only be locking the only existing local one.
 */
#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
static inline void __flags_noop(unsigned long *flags) { }
#define pcp_spin_lock_maybe_irqsave(ptr, flags)		\
({							\
	 __flags_noop(&(flags));			\
	 spin_lock(&(ptr)->lock);			\
})
#define pcp_spin_unlock_maybe_irqrestore(ptr, flags)	\
({							\
	 spin_unlock(&(ptr)->lock);			\
	 __flags_noop(&(flags));			\
})
#else
#define pcp_spin_lock_maybe_irqsave(ptr, flags)		\
		spin_lock_irqsave(&(ptr)->lock, flags)
#define pcp_spin_unlock_maybe_irqrestore(ptr, flags)	\
		spin_unlock_irqrestore(&(ptr)->lock, flags)
#endif

#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
DEFINE_PER_CPU(int, numa_node);
EXPORT_PER_CPU_SYMBOL(numa_node);
#endif

DEFINE_STATIC_KEY_TRUE(vm_numa_stat_key);

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * N.B., Do NOT reference the '_numa_mem_' per cpu variable directly.
 * It will not be defined when CONFIG_HAVE_MEMORYLESS_NODES is not defined.
 * Use the accessor functions set_numa_mem(), numa_mem_id() and cpu_to_mem()
 * defined in <linux/topology.h>.
 */
DEFINE_PER_CPU(int, _numa_mem_);		/* Kernel "local memory" node */
EXPORT_PER_CPU_SYMBOL(_numa_mem_);
#endif

static DEFINE_MUTEX(pcpu_drain_mutex);

#ifdef CONFIG_GCC_PLUGIN_LATENT_ENTROPY
volatile unsigned long latent_entropy __latent_entropy;
EXPORT_SYMBOL(latent_entropy);
#endif

/*
 * Array of node states.
 */
nodemask_t node_states[NR_NODE_STATES] __read_mostly = {
	[N_POSSIBLE] = NODE_MASK_ALL,
	[N_ONLINE] = { { [0] = 1UL } },
#ifndef CONFIG_NUMA
	[N_NORMAL_MEMORY] = { { [0] = 1UL } },
#ifdef CONFIG_HIGHMEM
	[N_HIGH_MEMORY] = { { [0] = 1UL } },
#endif
	[N_MEMORY] = { { [0] = 1UL } },
	[N_CPU] = { { [0] = 1UL } },
#endif	/* NUMA */
};
EXPORT_SYMBOL(node_states);

gfp_t gfp_allowed_mask __read_mostly = GFP_BOOT_MASK;

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE
unsigned int pageblock_order __read_mostly;
#endif

static void __free_pages_ok(struct page *page, unsigned int order,
			    fpi_t fpi_flags);

/*
 * results with 256, 32 in the lowmem_reserve sysctl:
 *	1G machine -> (16M dma, 800M-16M normal, 1G-800M high)
 *	1G machine -> (16M dma, 784M normal, 224M high)
 *	NORMAL allocation will leave 784M/256 of ram reserved in the ZONE_DMA
 *	HIGHMEM allocation will leave 224M/32 of ram reserved in ZONE_NORMAL
 *	HIGHMEM allocation will leave (224M+784M)/256 of ram reserved in ZONE_DMA
 *
 * TBD: should special case ZONE_DMA32 machines here - in those we normally
 * don't need any ZONE_NORMAL reservation
 */
static int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES] = {
#ifdef CONFIG_ZONE_DMA
	[ZONE_DMA] = 256,
#endif
#ifdef CONFIG_ZONE_DMA32
	[ZONE_DMA32] = 256,
#endif
	[ZONE_NORMAL] = 32,
#ifdef CONFIG_HIGHMEM
	[ZONE_HIGHMEM] = 0,
#endif
	[ZONE_MOVABLE] = 0,
};

char * const zone_names[MAX_NR_ZONES] = {
#ifdef CONFIG_ZONE_DMA
	 "DMA",
#endif
#ifdef CONFIG_ZONE_DMA32
	 "DMA32",
#endif
	 "Normal",
#ifdef CONFIG_HIGHMEM
	 "HighMem",
#endif
	 "Movable",
#ifdef CONFIG_ZONE_DEVICE
	 "Device",
#endif
};

const char * const migratetype_names[MIGRATE_TYPES] = {
	"Unmovable",
	"Movable",
	"Reclaimable",
	"HighAtomic",
#ifdef CONFIG_CMA
	"CMA",
#endif
#ifdef CONFIG_MEMORY_ISOLATION
	"Isolate",
#endif
};

int min_free_kbytes = 1024;
int user_min_free_kbytes = -1;
static int watermark_scale_factor = 10;
int defrag_mode;

/* movable_zone is the "real" zone pages in ZONE_MOVABLE are taken from */
int movable_zone;
EXPORT_SYMBOL(movable_zone);

#if MAX_NUMNODES > 1
unsigned int nr_node_ids __read_mostly = MAX_NUMNODES;
unsigned int nr_online_nodes __read_mostly = 1;
EXPORT_SYMBOL(nr_node_ids);
EXPORT_SYMBOL(nr_online_nodes);
#endif

static bool page_contains_unaccepted(struct page *page, unsigned int order);
static bool cond_accept_memory(struct zone *zone, unsigned int order,
			       int alloc_flags);
static bool __free_unaccepted(struct page *page);

int page_group_by_mobility_disabled __read_mostly;

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
/*
 * During boot we initialize deferred pages on-demand, as needed, but once
 * page_alloc_init_late() has finished, the deferred pages are all initialized,
 * and we can permanently disable that path.
 */
DEFINE_STATIC_KEY_TRUE(deferred_pages);

/*
 * deferred_grow_zone() is __init, but it is called from
 * get_page_from_freelist() during early boot until deferred_pages permanently
 * disables this call. This is why we have refdata wrapper to avoid warning,
 * and to ensure that the function body gets unloaded.
 */
static bool __ref
_deferred_grow_zone(struct zone *zone, unsigned int order)
{
	return deferred_grow_zone(zone, order);
}
#else
static inline bool _deferred_grow_zone(struct zone *zone, unsigned int order)
{
	return false;
}
#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

static __always_inline bool is_standalone_pb_bit(enum pageblock_bits pb_bit)
{
	return pb_bit >= PB_compact_skip && pb_bit < __NR_PAGEBLOCK_BITS;
}

static __always_inline unsigned long *
get_pfnblock_flags_word(const struct page *page, unsigned long pfn)
{
	BUILD_BUG_ON(__MIGRATE_TYPE_END > MIGRATETYPE_MASK);
	VM_BUG_ON_PAGE(!zone_spans_pfn(page_zone(page), pfn), page);

	return &pfn_to_pageblock(page, pfn)->flags;
}


/**
 * __get_pfnblock_flags_mask - Return the requested group of flags for
 * a pageblock_nr_pages block of pages
 * @page: The page within the block of interest
 * @pfn: The target page frame number
 * @mask: mask of bits that the caller is interested in
 *
 * Return: pageblock_bits flags
 */
static unsigned long __get_pfnblock_flags_mask(const struct page *page,
					       unsigned long pfn,
					       unsigned long mask)
{
	unsigned long *flags_word = get_pfnblock_flags_word(page, pfn);

	/*
	 * This races, without locks, with set_pfnblock_migratetype(). Ensure
	 * a consistent read of the memory array, so that results, even though
	 * racy, are not corrupted.
	 */
	return READ_ONCE(*flags_word) & mask;
}

/**
 * get_pfnblock_bit - Check if a standalone bit of a pageblock is set
 * @page: The page within the block of interest
 * @pfn: The target page frame number
 * @pb_bit: pageblock bit to check
 *
 * Return: true if the bit is set, otherwise false
 */
bool get_pfnblock_bit(const struct page *page, unsigned long pfn,
		      enum pageblock_bits pb_bit)
{
	if (WARN_ON_ONCE(!is_standalone_pb_bit(pb_bit)))
		return false;

	return test_bit(pb_bit, get_pfnblock_flags_word(page, pfn));
}

/*
 * Extract migratetype from a pageblock_data pointer. Callers that
 * already have the pbd can avoid a redundant pfn_to_pageblock().
 */
static __always_inline enum migratetype
pbd_migratetype(const struct pageblock_data *pbd)
{
	unsigned long flags = READ_ONCE(pbd->flags) & MIGRATETYPE_AND_ISO_MASK;

#ifdef CONFIG_MEMORY_ISOLATION
	if (flags & BIT(PB_migrate_isolate))
		return MIGRATE_ISOLATE;
#endif
	return flags & MIGRATETYPE_MASK;
}

/**
 * get_pfnblock_migratetype - Return the migratetype of a pageblock
 * @page: The page within the block of interest
 * @pfn: The target page frame number
 *
 * Return: The migratetype of the pageblock
 *
 * Use get_pfnblock_migratetype() if caller already has both @page and @pfn
 * to save a call to page_to_pfn().
 */
__always_inline enum migratetype
get_pfnblock_migratetype(const struct page *page, unsigned long pfn)
{
	return pbd_migratetype(pfn_to_pageblock(page, pfn));
}

/**
 * __set_pfnblock_flags_mask - Set the requested group of flags for
 * a pageblock_nr_pages block of pages
 * @page: The page within the block of interest
 * @pfn: The target page frame number
 * @flags: The flags to set
 * @mask: mask of bits that the caller is interested in
 */
static void __set_pfnblock_flags_mask(struct page *page, unsigned long pfn,
				      unsigned long flags, unsigned long mask)
{
	unsigned long *flags_word = get_pfnblock_flags_word(page, pfn);
	unsigned long word, new_word;

	word = READ_ONCE(*flags_word);
	do {
		new_word = (word & ~mask) | flags;
	} while (!try_cmpxchg(flags_word, &word, new_word));
}

/**
 * set_pfnblock_bit - Set a standalone bit of a pageblock
 * @page: The page within the block of interest
 * @pfn: The target page frame number
 * @pb_bit: pageblock bit to set
 */
void set_pfnblock_bit(const struct page *page, unsigned long pfn,
		      enum pageblock_bits pb_bit)
{
	if (WARN_ON_ONCE(!is_standalone_pb_bit(pb_bit)))
		return;

	set_bit(pb_bit, get_pfnblock_flags_word(page, pfn));
}

/**
 * clear_pfnblock_bit - Clear a standalone bit of a pageblock
 * @page: The page within the block of interest
 * @pfn: The target page frame number
 * @pb_bit: pageblock bit to clear
 */
void clear_pfnblock_bit(const struct page *page, unsigned long pfn,
			enum pageblock_bits pb_bit)
{
	if (WARN_ON_ONCE(!is_standalone_pb_bit(pb_bit)))
		return;

	clear_bit(pb_bit, get_pfnblock_flags_word(page, pfn));
}

/*
 * Map migratetype to PB_has_* bit index. Returns -1 for types that
 * don't have a tracking bit (e.g. MIGRATE_ISOLATE).
 */
static inline int migratetype_to_has_bit(int migratetype)
{
	switch (migratetype) {
	case MIGRATE_UNMOVABLE:
	case MIGRATE_HIGHATOMIC:
		return PB_has_unmovable;
	case MIGRATE_RECLAIMABLE:
		return PB_has_reclaimable;
	case MIGRATE_MOVABLE:
#ifdef CONFIG_CMA
	case MIGRATE_CMA:
#endif
		return PB_has_movable;
	default:
		return -1;
	}
}

#ifdef CONFIG_DEBUG_VM
static void spb_debug_check(struct superpageblock *sb, const char *caller)
{
	u16 total = sb->total_pageblocks;

	VM_WARN_ONCE(sb->nr_free > total,
		     "%s: nr_free %u > total %u (zone=%s sb=%lu)\n",
		     caller, sb->nr_free, total, sb->zone->name,
		     (unsigned long)(sb - sb->zone->superpageblocks));
	VM_WARN_ONCE(sb->nr_unmovable > total,
		     "%s: nr_unmovable %u > total %u (zone=%s sb=%lu)\n",
		     caller, sb->nr_unmovable, total, sb->zone->name,
		     (unsigned long)(sb - sb->zone->superpageblocks));
	VM_WARN_ONCE(sb->nr_reclaimable > total,
		     "%s: nr_reclaimable %u > total %u (zone=%s sb=%lu)\n",
		     caller, sb->nr_reclaimable, total, sb->zone->name,
		     (unsigned long)(sb - sb->zone->superpageblocks));
	VM_WARN_ONCE(sb->nr_movable > total,
		     "%s: nr_movable %u > total %u (zone=%s sb=%lu)\n",
		     caller, sb->nr_movable, total, sb->zone->name,
		     (unsigned long)(sb - sb->zone->superpageblocks));
}
#else
static inline void spb_debug_check(struct superpageblock *sb, const char *caller) {}
#endif

/*
 * __spb_set_has_type - set PB_has_* and increment type counter
 *
 * Idempotent: only increments the counter on the 0→1 bit transition.
 */
static void __spb_set_has_type(struct page *page, int migratetype)
{
	unsigned long pfn = page_to_pfn(page);
	struct superpageblock *sb = pfn_to_superpageblock(page_zone(page), pfn);
	int bit;

	if (!sb)
		return;

	bit = migratetype_to_has_bit(migratetype);
	if (bit < 0)
		return;

	if (!get_pfnblock_bit(page, pfn, bit)) {
		bool first = false;

		set_pfnblock_bit(page, pfn, bit);
		switch (bit) {
		case PB_has_unmovable:
			sb->nr_unmovable++;
			first = (sb->nr_unmovable == 1);
			if (first)
				trace_printk("SB first unmovable: zone=%s sb=%lu pfn=%lu mt=%d rsv=%u mov=%u recl=%u free=%u\n",
					     sb->zone->name,
					     (unsigned long)(sb - sb->zone->superpageblocks),
					     pfn, migratetype,
					     sb->nr_reserved, sb->nr_movable,
					     sb->nr_reclaimable, sb->nr_free);
			break;
		case PB_has_reclaimable:
			sb->nr_reclaimable++;
			first = (sb->nr_reclaimable == 1);
			if (first)
				trace_printk("SB first reclaimable: zone=%s sb=%lu pfn=%lu mt=%d rsv=%u mov=%u unmov=%u free=%u\n",
					     sb->zone->name,
					     (unsigned long)(sb - sb->zone->superpageblocks),
					     pfn, migratetype,
					     sb->nr_reserved, sb->nr_movable,
					     sb->nr_unmovable, sb->nr_free);
			break;
		case PB_has_movable:
			sb->nr_movable++;
			first = (sb->nr_movable == 1);
			break;
		}
		trace_spb_pb_taint(page, migratetype,
				   SPB_PB_TAINT_ACTION_SET, first);
		spb_debug_check(sb, "__spb_set_has_type");
	}
}

/*
 * __spb_clear_has_type - clear PB_has_* and decrement type counter
 *
 * Idempotent: only decrements the counter on the 1→0 bit transition.
 */
static void __spb_clear_has_type(struct page *page, int migratetype)
{
	unsigned long pfn = page_to_pfn(page);
	struct superpageblock *sb = pfn_to_superpageblock(page_zone(page), pfn);
	int bit;

	if (!sb)
		return;

	bit = migratetype_to_has_bit(migratetype);
	if (bit < 0)
		return;

	if (get_pfnblock_bit(page, pfn, bit)) {
		bool last = false;

		clear_pfnblock_bit(page, pfn, bit);
		switch (bit) {
		case PB_has_unmovable:
			if (sb->nr_unmovable)
				sb->nr_unmovable--;
			last = (sb->nr_unmovable == 0);
			break;
		case PB_has_reclaimable:
			if (sb->nr_reclaimable)
				sb->nr_reclaimable--;
			last = (sb->nr_reclaimable == 0);
			break;
		case PB_has_movable:
			if (sb->nr_movable)
				sb->nr_movable--;
			last = (sb->nr_movable == 0);
			break;
		}
		trace_spb_pb_taint(page, migratetype,
				   SPB_PB_TAINT_ACTION_CLEAR, last);
		spb_debug_check(sb, "__spb_clear_has_type");
	}
}

#ifdef CONFIG_COMPACTION
/*
 * spb_pageblock_has_free_movable_fragments - probe SPB free lists for movable
 * @zone: zone containing @page
 * @page: any page within the target pageblock
 *
 * Returns true if the SPB containing @page has any free MOVABLE pages on its
 * per-order free lists at orders below pageblock_order whose PFN falls within
 * the target pageblock. The compaction migrate scanner only sees in-use pages,
 * so a pageblock can look "empty of movable" to the scanner while the SPB
 * still owns small-order MOVABLE fragments inside it. Clearing PB_has_movable
 * in that case would orphan those fragments from the SPB type accounting and
 * trigger debugfs invariant 1 (sum_types undercount).
 *
 * Returns false (no fragments found) when the SPB lookup fails, which
 * preserves the legacy clear-on-empty behavior for edge cases.
 *
 * Caller must hold zone->lock.
 */
static bool spb_pageblock_has_free_movable_fragments(struct zone *zone,
						     struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long pb_start = pageblock_start_pfn(pfn);
	unsigned long pb_end = pb_start + pageblock_nr_pages;
	unsigned long frag_pfn;
	struct superpageblock *sb;
	struct list_head *list;
	struct page *frag;
	unsigned int order;

	sb = pfn_to_superpageblock(zone, pfn);
	if (!sb)
		return false;

	for (order = 0; order < pageblock_order; order++) {
		list = &sb->free_area[order].free_list[MIGRATE_MOVABLE];
		list_for_each_entry(frag, list, buddy_list) {
			frag_pfn = page_to_pfn(frag);
			if (frag_pfn >= pb_start && frag_pfn < pb_end)
				return true;
		}
	}

	return false;
}

/**
 * superpageblock_clear_has_movable - clear PB_has_movable with SPB counter update
 * @page: page within the pageblock
 *
 * Called from compaction when a full pageblock scan determines no movable
 * pages remain. Clears PB_has_movable and decrements the superpageblock's
 * nr_movable counter atomically (under zone->lock).
 *
 * Without this, clearing PB_has_movable directly via clear_pfnblock_bit()
 * would leave the SPB counter stale, causing nr_movable to grow unbounded
 * as subsequent movable allocations re-set the bit and re-increment.
 *
 * The migrate scanner only inspects in-use pages, so it is blind to MOVABLE
 * fragments below pageblock_order sitting on the SPB free lists. Probe those
 * lists first; if any fragment of @page's pageblock is still tracked by the
 * SPB, leave PB_has_movable set so the SPB type accounting stays consistent
 * (debugfs invariant 1: unmov + recl + mov + free >= total - rsv).
 */
void superpageblock_clear_has_movable(struct zone *zone, struct page *page)
{
	unsigned long flags;

	spin_lock_irqsave(&zone->lock, flags);
	if (!spb_pageblock_has_free_movable_fragments(zone, page))
		__spb_clear_has_type(page, MIGRATE_MOVABLE);
	spin_unlock_irqrestore(&zone->lock, flags);
}

/**
 * superpageblock_set_has_movable - set PB_has_movable with SPB counter update
 * @zone: zone containing the page
 * @page: page within the pageblock
 *
 * Called from compaction when a movable page is migrated into a pageblock.
 * Compaction bypasses page_del_and_expand (which normally sets PB_has_*)
 * by using __isolate_free_page + direct migration, so PB_has_movable must
 * be set explicitly for the destination pageblock.
 *
 * Idempotent: only increments the counter on the 0→1 bit transition.
 */
void superpageblock_set_has_movable(struct zone *zone, struct page *page)
{
	unsigned long flags;

	spin_lock_irqsave(&zone->lock, flags);
	__spb_set_has_type(page, MIGRATE_MOVABLE);
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif /* CONFIG_COMPACTION */

/**
 * spb_get_category - Determine if a superpageblock is clean or tainted
 * @sb: superpageblock to classify
 *
 * A superpageblock is clean if it contains only free and movable pageblocks.
 * Any unmovable, reclaimable, or reserved pageblocks make it tainted.
 * Reserved pageblocks (memory holes) taint the superpageblock because it
 * can never be used for 1GB hugepages, making it a better home for
 * unmovable/reclaimable allocations.
 */
static inline enum sb_category spb_get_category(struct superpageblock *sb)
{
	if (sb->nr_unmovable || sb->nr_reclaimable || sb->nr_reserved)
		return SB_TAINTED;
	return SB_CLEAN;
}

/**
 * sb_get_fullness - Determine the fullness bucket for a superpageblock
 * @sb: superpageblock to classify
 * @cat: the category (CLEAN or TAINTED) of this superpageblock
 *
 * For clean SPBs, fullness is based on total usage (total - nr_free).
 * For tainted SPBs, fullness is based only on unmovable + reclaimable
 * pageblocks, since those are what we're trying to concentrate.
 * Uses SUPERPAGEBLOCK_NR_PAGEBLOCKS as divisor so that partial
 * superpageblocks at zone boundaries are preferred over whole ones.
 */
static inline enum sb_fullness sb_get_fullness(struct superpageblock *sb,
					       enum sb_category cat)
{
	unsigned int used, total = sb->total_pageblocks;
	unsigned int quarter = SUPERPAGEBLOCK_NR_PAGEBLOCKS / 4;

	if (!total)
		return SB_FULL;

	if (cat == SB_TAINTED)
		used = sb->nr_unmovable + sb->nr_reclaimable;
	else
		used = total - sb->nr_free;

	if (used >= total)
		return SB_FULL;

	if (used >= 3 * quarter)
		return SB_FULL_75;
	if (used >= 2 * quarter)
		return SB_FULL_50;
	if (used >= quarter)
		return SB_FULL_25;
	return SB_ALMOST_EMPTY;
}

/**
 * spb_update_list - Move a superpageblock to the correct fullness list
 * @sb: superpageblock to reclassify
 *
 * Called after counters change. Removes from current list (if any)
 * and adds to the appropriate list based on current fullness and
 * taint status. Also triggers background defragmentation if the
 * superpageblock is tainted and running low on free space.
 */
#ifdef CONFIG_COMPACTION
static void spb_maybe_start_defrag(struct superpageblock *sb);
static bool spb_needs_defrag(struct superpageblock *sb);
static struct page *spb_try_alloc_contig(struct zone *zone,
					unsigned long nr_pages,
					gfp_t gfp_mask);
static bool spb_evacuate_for_order(struct zone *zone, unsigned int order,
				  int migratetype);
static void queue_spb_evacuate(struct zone *zone, unsigned int order,
			       int migratetype);
static void queue_spb_slab_shrink(struct zone *zone);
#else
static inline void spb_maybe_start_defrag(struct superpageblock *sb) {}
static inline bool spb_needs_defrag(struct superpageblock *sb) { return false; }
static inline struct page *spb_try_alloc_contig(struct zone *zone,
					       unsigned long nr_pages,
					       gfp_t gfp_mask)
{
	return NULL;
}
static inline bool spb_evacuate_for_order(struct zone *zone, unsigned int order,
					 int migratetype)
{
	return false;
}
static inline void queue_spb_evacuate(struct zone *zone, unsigned int order,
				      int migratetype) {}
static inline void queue_spb_slab_shrink(struct zone *zone) {}
#endif

static void spb_update_list(struct superpageblock *sb)
{
	struct zone *zone = sb->zone;
	enum sb_category cat;
	enum sb_fullness full;

	list_del_init(&sb->list);

	if (sb->contig_allocated) {
		list_add_tail(&sb->list, &zone->spb_isolated);
		return;
	}

	if (sb->nr_free == sb->total_pageblocks) {
		list_add_tail(&sb->list, &zone->spb_empty);
		return;
	}

	cat = spb_get_category(sb);
	full = sb_get_fullness(sb, cat);
	list_add_tail(&sb->list, &zone->spb_lists[cat][full]);

	spb_maybe_start_defrag(sb);
}

/**
 * superpageblock_pb_now_free - A pageblock just became fully free in buddy
 * @page: page in the pageblock
 *
 * When buddy coalescing reconstructs a complete pageblock-order page,
 * increment nr_free. Type counters are handled separately by
 * __spb_clear_has_type() in mark_pageblock_free().
 */
static void superpageblock_pb_now_free(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct superpageblock *sb = pfn_to_superpageblock(page_zone(page), pfn);

	if (!sb)
		return;

	sb->nr_free++;
	spb_debug_check(sb, "pb_now_free");

	spb_update_list(sb);
}

/**
 * superpageblock_pb_now_used - A fully-free pageblock just got its first allocation
 * @page: page in the pageblock
 *
 * When allocating from an order >= pageblock_order free page, decrement
 * nr_free. Type counters are handled separately by __spb_set_has_type()
 * at allocation time.
 */
static void superpageblock_pb_now_used(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct superpageblock *sb = pfn_to_superpageblock(page_zone(page), pfn);

	if (!sb)
		return;

	if (sb->nr_free)
		sb->nr_free--;
	spb_debug_check(sb, "pb_now_used");

	spb_update_list(sb);
}

/**
 * superpageblock_range_now_used - Mark a multi-pageblock free range as no longer free
 * @page: first page of the range (must be pageblock-aligned)
 * @order: order of the range (must be >= pageblock_order)
 *
 * When a free page of order >= pageblock_order is removed from buddy outside
 * the normal allocation path (e.g. __isolate_free_page, memory hotplug,
 * HW poison takeoff), every constituent pageblock leaves its PB_all_free
 * state. Walk the range, clear PB_all_free, and decrement nr_free for each
 * affected pageblock. PB_has_* bits are not touched: the pages are not being
 * allocated to a specific migratetype here. They will be re-established by
 * mark_pageblock_free() if the pages later return to buddy and coalesce.
 */
static void superpageblock_range_now_used(struct page *page, unsigned int order)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long end_pfn = pfn + (1UL << order);

	for (; pfn < end_pfn; pfn += pageblock_nr_pages) {
		struct page *pb_page = pfn_to_page(pfn);

		if (get_pfnblock_bit(pb_page, pfn, PB_all_free)) {
			clear_pfnblock_bit(pb_page, pfn, PB_all_free);
			superpageblock_pb_now_used(pb_page);
		}
	}
}

/** set_pageblock_migratetype - Set the migratetype of a pageblock
 * @page: The page within the block of interest
 * @migratetype: migratetype to set
 */
static void set_pageblock_migratetype(struct page *page,
				      enum migratetype migratetype)
{
	if (unlikely(page_group_by_mobility_disabled &&
		     migratetype < MIGRATE_PCPTYPES))
		migratetype = MIGRATE_UNMOVABLE;

#ifdef CONFIG_MEMORY_ISOLATION
	if (migratetype == MIGRATE_ISOLATE) {
		VM_WARN_ONCE(1,
			"Use set_pageblock_isolate() for pageblock isolation");
		return;
	}
	VM_WARN_ONCE(get_pageblock_isolate(page),
		     "Use clear_pageblock_isolate() to unisolate pageblock");
	/* MIGRATETYPE_AND_ISO_MASK clears PB_migrate_isolate if it is set */
#endif
	__set_pfnblock_flags_mask(page, page_to_pfn(page),
				  (unsigned long)migratetype,
				  MIGRATETYPE_AND_ISO_MASK);
}

void __meminit init_pageblock_migratetype(struct page *page,
					  enum migratetype migratetype,
					  bool isolate)
{
	unsigned long pfn = page_to_pfn(page);
	struct pageblock_data *pbd;
	struct superpageblock *sb;
	unsigned long flags;

	if (unlikely(page_group_by_mobility_disabled &&
		     migratetype < MIGRATE_PCPTYPES))
		migratetype = MIGRATE_UNMOVABLE;

	flags = migratetype;

#ifdef CONFIG_MEMORY_ISOLATION
	if (migratetype == MIGRATE_ISOLATE) {
		VM_WARN_ONCE(
			1,
			"Set isolate=true to isolate pageblock with a migratetype");
		return;
	}
	if (isolate)
		flags |= BIT(PB_migrate_isolate);
#endif
	__set_pfnblock_flags_mask(page, pfn, flags, MIGRATETYPE_AND_ISO_MASK);

	pbd = pfn_to_pageblock(page, pfn);
	pbd->block_pfn = pfn;
	INIT_LIST_HEAD(&pbd->cpu_node);

	/* Transition from reserved (boot default) to initial migratetype */
	sb = pfn_to_superpageblock(page_zone(page), pfn);
	if (sb) {
		if (sb->nr_reserved)
			sb->nr_reserved--;
		__spb_set_has_type(page, migratetype);
		spb_update_list(sb);
	}
}

#ifdef CONFIG_CONTIG_ALLOC
/**
 * superpageblock_contig_mark - Mark/unmark SPBs for contiguous allocation
 * @start: start PFN of the contiguous range
 * @end: end PFN (exclusive) of the contiguous range
 * @allocated: true when allocated, false when freed
 *
 * Called after a successful contiguous allocation (or before freeing) to
 * mark fully-covered superpageblocks as contig_allocated. This moves them
 * to the spb_isolated list so they don't participate in allocation steering,
 * and makes them visible in debugfs.
 */
static void superpageblock_contig_mark(unsigned long start, unsigned long end,
				       bool allocated)
{
	struct zone *zone = page_zone(pfn_to_page(start));
	unsigned long spb_pages = SUPERPAGEBLOCK_NR_PAGES;
	unsigned long pfn;
	unsigned long flags;

	/* Only track full-SPB contiguous allocations */
	if (end - start < spb_pages)
		return;

	spin_lock_irqsave(&zone->lock, flags);
	for (pfn = ALIGN(start, spb_pages); pfn + spb_pages <= end;
	     pfn += spb_pages) {
		struct superpageblock *sb = pfn_to_superpageblock(zone, pfn);

		if (!sb)
			continue;

		sb->contig_allocated = allocated;
		spb_update_list(sb);
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif /* CONFIG_CONTIG_ALLOC */

#ifdef CONFIG_DEBUG_VM
static int page_outside_zone_boundaries(struct zone *zone, struct page *page)
{
	int ret;
	unsigned seq;
	unsigned long pfn = page_to_pfn(page);
	unsigned long sp, start_pfn;

	do {
		seq = zone_span_seqbegin(zone);
		start_pfn = zone->zone_start_pfn;
		sp = zone->spanned_pages;
		ret = !zone_spans_pfn(zone, pfn);
	} while (zone_span_seqretry(zone, seq));

	if (ret)
		pr_err("page 0x%lx outside node %d zone %s [ 0x%lx - 0x%lx ]\n",
			pfn, zone_to_nid(zone), zone->name,
			start_pfn, start_pfn + sp);

	return ret;
}

/*
 * Temporary debugging check for pages not lying within a given zone.
 */
static bool __maybe_unused bad_range(struct zone *zone, struct page *page)
{
	if (page_outside_zone_boundaries(zone, page))
		return true;
	if (zone != page_zone(page))
		return true;

	return false;
}
#else
static inline bool __maybe_unused bad_range(struct zone *zone, struct page *page)
{
	return false;
}
#endif

static void bad_page(struct page *page, const char *reason)
{
	static unsigned long resume;
	static unsigned long nr_shown;
	static unsigned long nr_unshown;

	/*
	 * Allow a burst of 60 reports, then keep quiet for that minute;
	 * or allow a steady drip of one report per second.
	 */
	if (nr_shown == 60) {
		if (time_before(jiffies, resume)) {
			nr_unshown++;
			goto out;
		}
		if (nr_unshown) {
			pr_alert(
			      "BUG: Bad page state: %lu messages suppressed\n",
				nr_unshown);
			nr_unshown = 0;
		}
		nr_shown = 0;
	}
	if (nr_shown++ == 0)
		resume = jiffies + 60 * HZ;

	pr_alert("BUG: Bad page state in process %s  pfn:%05lx\n",
		current->comm, page_to_pfn(page));
	dump_page(page, reason);

	print_modules();
	dump_stack();
out:
	/* Leave bad fields for debug, except PageBuddy could make trouble */
	if (PageBuddy(page))
		__ClearPageBuddy(page);
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
}

static inline unsigned int order_to_pindex(int migratetype, int order)
{
	VM_BUG_ON(order > PAGE_BLOCK_MAX_ORDER);

	return (MIGRATE_PCPTYPES * order) + migratetype;
}

static inline int pindex_to_order(unsigned int pindex)
{
	int order = pindex / MIGRATE_PCPTYPES;

	VM_BUG_ON(order > PAGE_BLOCK_MAX_ORDER);

	return order;
}

static inline bool pcp_allowed_order(unsigned int order)
{
	return order <= pageblock_order;
}

/*
 * Higher-order pages are called "compound pages".  They are structured thusly:
 *
 * The first PAGE_SIZE page is called the "head page" and have PG_head set.
 *
 * The remaining PAGE_SIZE pages are called "tail pages". PageTail() is encoded
 * in bit 0 of page->compound_head. The rest of bits is pointer to head page.
 *
 * The first tail page's ->compound_order holds the order of allocation.
 * This usage means that zero-order pages may not be compound.
 */

void prep_compound_page(struct page *page, unsigned int order)
{
	int i;
	int nr_pages = 1 << order;

	__SetPageHead(page);
	for (i = 1; i < nr_pages; i++)
		prep_compound_tail(page, i);

	prep_compound_head(page, order);
}

static inline void set_buddy_order(struct page *page, unsigned int order)
{
	set_page_private(page, order);
	__SetPageBuddy(page);
}

/*
 * PCP pageblock ownership tracking.
 *
 * Ownership rules:
 * - Whole pageblocks acquired by rmqueue_bulk() Phase 1 are owned, meaning
 *   all frees will be routed to that PCP.
 * - Draining a whole pageblock back to the zone clears PCP ownership.
 * - Draining a partial block (due to PCP thresholds or memory pressure) puts
 *   the block on the pcp->owned_blocks list. A later refill will attempt to
 *   recover it in Phase 0.
 * - Whole pageblocks can assemble on the zone buddy due to PCP bypasses,
 *   e.g. during lock contention. __free_one_page() clears stale ownership.
 * - Phases 2/3 refill with fragments for pure caching - if there are not
 *   enough blocks or pcp->high restrictions. They do not participate
 *   in ownership, affinity enforcement, or on-PCP merging.
 *
 * PagePCPBuddy means "mergeable buddy on home PCP":
 * - Set when Phase 0/1 restore or acquire whole pageblocks.
 * - Propagated to split remainders in pcp_rmqueue_smallest().
 * - Set on freed pages from owned blocks routed to the owner PCP.
 * - NOT set for Phase 3 fragments or zone-owned frees.
 * - The merge pass in free_pcppages_bulk() only processes
 *   PagePCPBuddy pages, ensuring it never touches pages on
 *   another CPU's PCP list.
 *
 * We store the owning CPU + 1, so the default value of 0 in those
 * arrays means no owner / zone owner (and not CPU 0).
 */

static inline void clear_pcpblock_owner(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct pageblock_data *pbd = pfn_to_pageblock(page, pfn);

	pbd->cpu = 0;
	list_del_init(&pbd->cpu_node);
}

static inline void set_pcpblock_owner(struct page *page, int cpu)
{
	pfn_to_pageblock(page, page_to_pfn(page))->cpu = cpu + 1;
}

static inline int get_pcpblock_owner(struct page *page)
{
	return pfn_to_pageblock(page, page_to_pfn(page))->cpu - 1;
}

static inline void set_pcp_order(struct page *page, unsigned int order)
{
	set_page_private(page, order);
}

static inline unsigned int pcp_buddy_order(struct page *page)
{
	return page_private(page);
}

static void pcp_enqueue(struct per_cpu_pages *pcp, struct page *page,
			int migratetype, unsigned int order)
{
	set_pcp_order(page, order);
	list_add(&page->pcp_list,
		 &pcp->lists[order_to_pindex(migratetype, order)]);
	pcp->count += 1 << order;
}

static void pcp_enqueue_tail(struct per_cpu_pages *pcp, struct page *page,
			     int migratetype, unsigned int order)
{
	set_pcp_order(page, order);
	list_add_tail(&page->pcp_list,
		      &pcp->lists[order_to_pindex(migratetype, order)]);
	pcp->count += 1 << order;
}

static void pcp_dequeue(struct per_cpu_pages *pcp, struct page *page,
			unsigned int order)
{
	list_del(&page->pcp_list);
	__ClearPagePCPBuddy(page);
	set_page_private(page, 0);
	pcp->count -= 1 << order;
}

#ifdef CONFIG_COMPACTION
static inline struct capture_control *task_capc(struct zone *zone)
{
	struct capture_control *capc = current->capture_control;

	return unlikely(capc) &&
		!(current->flags & PF_KTHREAD) &&
		!capc->page &&
		capc->cc->zone == zone ? capc : NULL;
}

static inline bool
compaction_capture(struct capture_control *capc, struct page *page,
		   int order, int migratetype)
{
	if (!capc || order != capc->cc->order)
		return false;

	/* Do not accidentally pollute CMA or isolated regions*/
	if (is_migrate_cma(migratetype) ||
	    is_migrate_isolate(migratetype))
		return false;

	/*
	 * Do not let lower order allocations pollute a movable pageblock
	 * unless compaction is also requesting movable pages.
	 * This might let an unmovable request use a reclaimable pageblock
	 * and vice-versa but no more than normal fallback logic which can
	 * have trouble finding a high-order free page.
	 */
	if (order < pageblock_order && migratetype == MIGRATE_MOVABLE &&
	    capc->cc->migratetype != MIGRATE_MOVABLE)
		return false;

	if (migratetype != capc->cc->migratetype)
		trace_mm_page_alloc_extfrag(page, capc->cc->order, order,
					    capc->cc->migratetype, migratetype);

	capc->page = page;
	return true;
}

#else
static inline struct capture_control *task_capc(struct zone *zone)
{
	return NULL;
}

static inline bool
compaction_capture(struct capture_control *capc, struct page *page,
		   int order, int migratetype)
{
	return false;
}
#endif /* CONFIG_COMPACTION */

static inline void account_freepages(struct zone *zone, int nr_pages,
				     int migratetype)
{
	lockdep_assert_held(&zone->lock);

	if (is_migrate_isolate(migratetype))
		return;

	__mod_zone_page_state(zone, NR_FREE_PAGES, nr_pages);

	if (is_migrate_cma(migratetype))
		__mod_zone_page_state(zone, NR_FREE_CMA_PAGES, nr_pages);
	else if (migratetype == MIGRATE_HIGHATOMIC)
		WRITE_ONCE(zone->nr_free_highatomic,
			   zone->nr_free_highatomic + nr_pages);
}

/**
 * pfn_sb_free_area - Get the correct free_area for a page at given order
 * @zone: the zone
 * @pfn: page frame number
 * @order: buddy order
 *
 * Returns the per-superpageblock free_area if the page belongs to a valid
 * superpageblock. Otherwise returns the zone free_area (for zones where the
 * superpageblock setup failed).
 */
static inline struct free_area *pfn_sb_free_area(struct zone *zone,
						 unsigned long pfn,
						 unsigned int order,
						 struct superpageblock **sbp)
{
	struct superpageblock *sb = pfn_to_superpageblock(zone, pfn);

	if (sb) {
		if (sbp)
			*sbp = sb;
		return &sb->free_area[order];
	}
	if (sbp)
		*sbp = NULL;
	return &zone->free_area[order];
}

/* Used for pages not on another list */
static inline void __add_to_free_list(struct page *page, struct zone *zone,
				      unsigned int order, int migratetype,
				      bool tail)
{
	unsigned long pfn = page_to_pfn(page);
	struct superpageblock *sb;
	struct free_area *area = pfn_sb_free_area(zone, pfn, order, &sb);
	int nr_pages = 1 << order;

	VM_WARN_ONCE(get_pageblock_migratetype(page) != migratetype &&
		     !is_migrate_isolate(get_pageblock_migratetype(page)) &&
		     !is_migrate_cma(get_pageblock_migratetype(page)),
		     "page type is %d, passed migratetype is %d (nr=%d)\n",
		     get_pageblock_migratetype(page), migratetype, nr_pages);

	if (tail)
		list_add_tail(&page->buddy_list, &area->free_list[migratetype]);
	else
		list_add(&page->buddy_list, &area->free_list[migratetype]);
	area->nr_free++;

	if (sb) {
		/* Keep zone-level nr_free accurate for watermark checks */
		zone->free_area[order].nr_free++;
		/* Track total free pages per superpageblock */
		sb->nr_free_pages += nr_pages;
	}

	if (order >= pageblock_order && !is_migrate_isolate(migratetype))
		__mod_zone_page_state(zone, NR_FREE_PAGES_BLOCKS, nr_pages);
}

/*
 * Used for pages which are on another list. Move the pages to the tail
 * of the list - so the moved pages won't immediately be considered for
 * allocation again (e.g., optimization for memory onlining).
 */
static inline void move_to_free_list(struct page *page, struct zone *zone,
				     unsigned int order, int old_mt, int new_mt)
{
	unsigned long pfn = page_to_pfn(page);
	struct free_area *area = pfn_sb_free_area(zone, pfn, order, NULL);
	int nr_pages = 1 << order;

	/* Free page moving can fail, so it happens before the type update */
	VM_WARN_ONCE(get_pageblock_migratetype(page) != old_mt &&
		     !is_migrate_cma(get_pageblock_migratetype(page)),
		     "page type is %d, passed migratetype is %d (nr=%d)\n",
		     get_pageblock_migratetype(page), old_mt, nr_pages);

	list_move_tail(&page->buddy_list, &area->free_list[new_mt]);

	account_freepages(zone, -nr_pages, old_mt);
	account_freepages(zone, nr_pages, new_mt);

	if (order >= pageblock_order &&
	    is_migrate_isolate(old_mt) != is_migrate_isolate(new_mt)) {
		if (!is_migrate_isolate(old_mt))
			nr_pages = -nr_pages;
		__mod_zone_page_state(zone, NR_FREE_PAGES_BLOCKS, nr_pages);
	}
}

static inline void __del_page_from_free_list(struct page *page, struct zone *zone,
					     unsigned int order, int migratetype)
{
	unsigned long pfn = page_to_pfn(page);
	struct superpageblock *sb;
	struct free_area *area = pfn_sb_free_area(zone, pfn, order, &sb);
	int nr_pages = 1 << order;

	VM_WARN_ONCE(get_pageblock_migratetype(page) != migratetype &&
		     !is_migrate_isolate(get_pageblock_migratetype(page)) &&
		     !is_migrate_cma(get_pageblock_migratetype(page)),
		     "page type is %d, passed migratetype is %d (nr=%d)\n",
		     get_pageblock_migratetype(page), migratetype, nr_pages);

	/* clear reported state and update reported page count */
	if (page_reported(page))
		__ClearPageReported(page);

	list_del(&page->buddy_list);
	__ClearPageBuddy(page);
	set_page_private(page, 0);
	area->nr_free--;

	if (sb) {
		/* Keep zone-level nr_free accurate for watermark checks */
		zone->free_area[order].nr_free--;
		/* Track total free pages per superpageblock */
		sb->nr_free_pages -= nr_pages;
	}

	if (order >= pageblock_order && !is_migrate_isolate(migratetype))
		__mod_zone_page_state(zone, NR_FREE_PAGES_BLOCKS, -nr_pages);
}

static inline void del_page_from_free_list(struct page *page, struct zone *zone,
					   unsigned int order, int migratetype)
{
	__del_page_from_free_list(page, zone, order, migratetype);
	account_freepages(zone, -(1 << order), migratetype);
}

static inline struct page *get_page_from_free_area(struct free_area *area,
					    int migratetype)
{
	return list_first_entry_or_null(&area->free_list[migratetype],
					struct page, buddy_list);
}

/*
 * If this is less than the 2nd largest possible page, check if the buddy
 * of the next-higher order is free. If it is, it's possible
 * that pages are being freed that will coalesce soon. In case,
 * that is happening, add the free page to the tail of the list
 * so it's less likely to be used soon and more likely to be merged
 * as a 2-level higher order page
 */
static inline bool
buddy_merge_likely(unsigned long pfn, unsigned long buddy_pfn,
		   struct page *page, unsigned int order)
{
	unsigned long higher_page_pfn;
	struct page *higher_page;

	if (order >= MAX_PAGE_ORDER - 1)
		return false;

	higher_page_pfn = buddy_pfn & pfn;
	higher_page = page + (higher_page_pfn - pfn);

	return find_buddy_page_pfn(higher_page, higher_page_pfn, order + 1,
			NULL) != NULL;
}

static void change_pageblock_range(struct page *pageblock_page,
				   int start_order, int migratetype)
{
	int nr_pageblocks = 1 << (start_order - pageblock_order);

	while (nr_pageblocks--) {
		set_pageblock_migratetype(pageblock_page, migratetype);
		pageblock_page += pageblock_nr_pages;
	}
}

/**
 * mark_pageblock_free - handle a pageblock becoming fully free
 * @page: page at the start of the pageblock
 * @pfn: page frame number
 * @migratetype: pointer to the caller's migratetype variable (may be updated)
 *
 * Clear stale PCP ownership and actual-contents tracking flags, mark the
 * pageblock as fully free for superpageblock accounting, and reset the
 * migratetype to MOVABLE so the page lands on free_list[MIGRATE_MOVABLE].
 * Non-movable allocations must go through RMQUEUE_CLAIM to reuse it,
 * which properly handles PB_all_free and superpageblock accounting.
 */
static void mark_pageblock_free(struct page *page, unsigned long pfn,
				int *migratetype)
{
	clear_pcpblock_owner(page);

	/*
	 * Clear PB_has_* bits and decrement corresponding SPB type
	 * counters. Use __spb_clear_has_type (no list update) to avoid
	 * bouncing the SPB between lists; pb_now_free's spb_update_list
	 * handles the final reclassification.
	 */
	__spb_clear_has_type(page, MIGRATE_UNMOVABLE);
	__spb_clear_has_type(page, MIGRATE_RECLAIMABLE);
	__spb_clear_has_type(page, MIGRATE_MOVABLE);

	if (!get_pfnblock_bit(page, pfn, PB_all_free)) {
		set_pfnblock_bit(page, pfn, PB_all_free);
		superpageblock_pb_now_free(page);
	}

	if (*migratetype == MIGRATE_UNMOVABLE ||
	    *migratetype == MIGRATE_RECLAIMABLE ||
	    *migratetype == MIGRATE_HIGHATOMIC) {
		set_pageblock_migratetype(page, MIGRATE_MOVABLE);
		*migratetype = MIGRATE_MOVABLE;
	}
}

/*
 * Freeing function for a buddy system allocator.
 *
 * The concept of a buddy system is to maintain direct-mapped table
 * (containing bit values) for memory blocks of various "orders".
 * The bottom level table contains the map for the smallest allocatable
 * units of memory (here, pages), and each level above it describes
 * pairs of units from the levels below, hence, "buddies".
 * At a high level, all that happens here is marking the table entry
 * at the bottom level available, and propagating the changes upward
 * as necessary, plus some accounting needed to play nicely with other
 * parts of the VM system.
 * At each level, we keep a list of pages, which are heads of continuous
 * free pages of length of (1 << order) and marked with PageBuddy.
 * Page's order is recorded in page_private(page) field.
 * So when we are allocating or freeing one, we can derive the state of the
 * other.  That is, if we allocate a small block, and both were
 * free, the remainder of the region must be split into blocks.
 * If a block is freed, and its buddy is also free, then this
 * triggers coalescing into a block of larger size.
 *
 * -- nyc
 */

static inline void __free_one_page(struct page *page,
		unsigned long pfn,
		struct zone *zone, unsigned int order,
		int migratetype, fpi_t fpi_flags)
{
	struct capture_control *capc = task_capc(zone);
	unsigned int orig_order = order;
	unsigned long buddy_pfn = 0;
	unsigned long combined_pfn;
	struct page *buddy;
	bool to_tail;

	VM_BUG_ON(!zone_is_initialized(zone));
	VM_BUG_ON_PAGE(page->flags.f & PAGE_FLAGS_CHECK_AT_PREP, page);

	VM_BUG_ON(migratetype == -1);
	VM_BUG_ON_PAGE(pfn & ((1 << order) - 1), page);
	VM_BUG_ON_PAGE(bad_range(zone, page), page);

	if (order >= pageblock_order) {
		int i, nr_pbs = 1 << (order - pageblock_order);

		for (i = 0; i < nr_pbs; i++) {
			int pb_mt = get_pfnblock_migratetype(
					page + i * pageblock_nr_pages,
					pfn + i * pageblock_nr_pages);
			mark_pageblock_free(page + i * pageblock_nr_pages,
					    pfn + i * pageblock_nr_pages,
					    &pb_mt);
		}
		/*
		 * After mark_pageblock_free, non-CMA sub-pageblocks are
		 * MOVABLE. CMA pageblocks retain their CMA type so pages
		 * land on the correct free list for CMA allocations.
		 * ISOLATE pageblocks must stay ISOLATE so that
		 * account_freepages() correctly skips them — otherwise
		 * NR_FREE_PAGES gets incremented for isolated pages.
		 */
		if (!is_migrate_cma(migratetype) &&
		    !is_migrate_isolate(migratetype))
			migratetype = MIGRATE_MOVABLE;
	}

	account_freepages(zone, 1 << order, migratetype);

	while (order < MAX_PAGE_ORDER) {
		int buddy_mt = migratetype;

		if (compaction_capture(capc, page, order, migratetype)) {
			account_freepages(zone, -(1 << order), migratetype);
			return;
		}

		buddy = find_buddy_page_pfn(page, pfn, order, &buddy_pfn);
		if (!buddy)
			goto done_merging;

		if (unlikely(order >= pageblock_order)) {
			/*
			 * We want to prevent merge between freepages on pageblock
			 * without fallbacks and normal pageblock. Without this,
			 * pageblock isolation could cause incorrect freepage or CMA
			 * accounting or HIGHATOMIC accounting.
			 */
			buddy_mt = get_pfnblock_migratetype(buddy, buddy_pfn);

			if (migratetype != buddy_mt &&
			    (!migratetype_is_mergeable(migratetype) ||
			     !migratetype_is_mergeable(buddy_mt)))
				goto done_merging;
		}

		/*
		 * Our buddy is free or it is CONFIG_DEBUG_PAGEALLOC guard page,
		 * merge with it and move up one order.
		 */
		if (page_is_guard(buddy))
			clear_page_guard(zone, buddy, order);
		else
			__del_page_from_free_list(buddy, zone, order, buddy_mt);

		if (unlikely(buddy_mt != migratetype)) {
			/*
			 * Match buddy type. This ensures that an
			 * expand() down the line puts the sub-blocks
			 * on the right freelists.
			 */
			change_pageblock_range(buddy, order, migratetype);
		}

		combined_pfn = buddy_pfn & pfn;
		page = page + (combined_pfn - pfn);
		pfn = combined_pfn;
		order++;

		/*
		 * If merging has reconstructed a full pageblock,
		 * clear any stale PCP ownership and actual-contents
		 * tracking flags.
		 */
		if (order == pageblock_order) {
			int old_mt = migratetype;

			mark_pageblock_free(page, pfn, &migratetype);
			/*
			 * mark_pageblock_free may convert migratetype to
			 * MOVABLE. Transfer the accounting done earlier so
			 * nr_free_highatomic doesn't leak.
			 *
			 * We transfer 1 << orig_order pages — the amount
			 * credited by this __free_one_page call. Buddies
			 * consumed during merging may also have HIGHATOMIC
			 * credits from their own frees; those are not tracked
			 * here. In practice HIGHATOMIC reserves are small and
			 * short-lived, so any residual drift is minor.
			 */
			if (old_mt != migratetype) {
				account_freepages(zone, -(1 << orig_order),
						  old_mt);
				account_freepages(zone, 1 << orig_order,
						  migratetype);
			}
		}
	}

done_merging:
	set_buddy_order(page, order);

	if (fpi_flags & FPI_TO_TAIL)
		to_tail = true;
	else if (is_shuffle_order(order))
		to_tail = shuffle_pick_tail();
	else
		to_tail = buddy_merge_likely(pfn, buddy_pfn, page, order);

	__add_to_free_list(page, zone, order, migratetype, to_tail);

	/* Notify page reporting subsystem of freed page */
	if (!(fpi_flags & FPI_SKIP_REPORT_NOTIFY))
		page_reporting_notify_free(order);
}

/*
 * A bad page could be due to a number of fields. Instead of multiple branches,
 * try and check multiple fields with one check. The caller must do a detailed
 * check if necessary.
 */
static inline bool page_expected_state(struct page *page,
					unsigned long check_flags)
{
	if (unlikely(atomic_read(&page->_mapcount) != -1))
		return false;

	if (unlikely((unsigned long)page->mapping |
			page_ref_count(page) |
#ifdef CONFIG_MEMCG
			page->memcg_data |
#endif
			page_pool_page_is_pp(page) |
			(page->flags.f & check_flags)))
		return false;

	return true;
}

static const char *page_bad_reason(struct page *page, unsigned long flags)
{
	const char *bad_reason = NULL;

	if (unlikely(atomic_read(&page->_mapcount) != -1))
		bad_reason = "nonzero mapcount";
	if (unlikely(page->mapping != NULL))
		bad_reason = "non-NULL mapping";
	if (unlikely(page_ref_count(page) != 0))
		bad_reason = "nonzero _refcount";
	if (unlikely(page->flags.f & flags)) {
		if (flags == PAGE_FLAGS_CHECK_AT_PREP)
			bad_reason = "PAGE_FLAGS_CHECK_AT_PREP flag(s) set";
		else
			bad_reason = "PAGE_FLAGS_CHECK_AT_FREE flag(s) set";
	}
#ifdef CONFIG_MEMCG
	if (unlikely(page->memcg_data))
		bad_reason = "page still charged to cgroup";
#endif
	if (unlikely(page_pool_page_is_pp(page)))
		bad_reason = "page_pool leak";
	return bad_reason;
}

static inline bool free_page_is_bad(struct page *page)
{
	if (likely(page_expected_state(page, PAGE_FLAGS_CHECK_AT_FREE)))
		return false;

	/* Something has gone sideways, find it */
	bad_page(page, page_bad_reason(page, PAGE_FLAGS_CHECK_AT_FREE));
	return true;
}

static inline bool is_check_pages_enabled(void)
{
	return static_branch_unlikely(&check_pages_enabled);
}

static int free_tail_page_prepare(struct page *head_page, struct page *page)
{
	struct folio *folio = (struct folio *)head_page;
	int ret = 1;

	/*
	 * We rely page->lru.next never has bit 0 set, unless the page
	 * is PageTail(). Let's make sure that's true even for poisoned ->lru.
	 */
	BUILD_BUG_ON((unsigned long)LIST_POISON1 & 1);

	if (!is_check_pages_enabled()) {
		ret = 0;
		goto out;
	}
	switch (page - head_page) {
	case 1:
		/* the first tail page: these may be in place of ->mapping */
		if (unlikely(folio_large_mapcount(folio))) {
			bad_page(page, "nonzero large_mapcount");
			goto out;
		}
		if (IS_ENABLED(CONFIG_PAGE_MAPCOUNT) &&
		    unlikely(atomic_read(&folio->_nr_pages_mapped))) {
			bad_page(page, "nonzero nr_pages_mapped");
			goto out;
		}
		if (IS_ENABLED(CONFIG_MM_ID)) {
			if (unlikely(folio->_mm_id_mapcount[0] != -1)) {
				bad_page(page, "nonzero mm mapcount 0");
				goto out;
			}
			if (unlikely(folio->_mm_id_mapcount[1] != -1)) {
				bad_page(page, "nonzero mm mapcount 1");
				goto out;
			}
		}
		if (IS_ENABLED(CONFIG_64BIT)) {
			if (unlikely(atomic_read(&folio->_entire_mapcount) + 1)) {
				bad_page(page, "nonzero entire_mapcount");
				goto out;
			}
			if (unlikely(atomic_read(&folio->_pincount))) {
				bad_page(page, "nonzero pincount");
				goto out;
			}
		}
		break;
	case 2:
		/* the second tail page: deferred_list overlaps ->mapping */
		if (unlikely(!list_empty(&folio->_deferred_list))) {
			bad_page(page, "on deferred list");
			goto out;
		}
		if (!IS_ENABLED(CONFIG_64BIT)) {
			if (unlikely(atomic_read(&folio->_entire_mapcount) + 1)) {
				bad_page(page, "nonzero entire_mapcount");
				goto out;
			}
			if (unlikely(atomic_read(&folio->_pincount))) {
				bad_page(page, "nonzero pincount");
				goto out;
			}
		}
		break;
	case 3:
		/* the third tail page: hugetlb specifics overlap ->mappings */
		if (IS_ENABLED(CONFIG_HUGETLB_PAGE))
			break;
		fallthrough;
	default:
		if (page->mapping != TAIL_MAPPING) {
			bad_page(page, "corrupted mapping in tail page");
			goto out;
		}
		break;
	}
	if (unlikely(!PageTail(page))) {
		bad_page(page, "PageTail not set");
		goto out;
	}
	if (unlikely(compound_head(page) != head_page)) {
		bad_page(page, "compound_head not consistent");
		goto out;
	}
	ret = 0;
out:
	page->mapping = NULL;
	clear_compound_head(page);
	return ret;
}

/*
 * Skip KASAN memory poisoning when either:
 *
 * 1. For generic KASAN: deferred memory initialization has not yet completed.
 *    Tag-based KASAN modes skip pages freed via deferred memory initialization
 *    using page tags instead (see below).
 * 2. For tag-based KASAN modes: the page has a match-all KASAN tag, indicating
 *    that error detection is disabled for accesses via the page address.
 *
 * Pages will have match-all tags in the following circumstances:
 *
 * 1. Pages are being initialized for the first time, including during deferred
 *    memory init; see the call to page_kasan_tag_reset in __init_single_page.
 * 2. The allocation was not unpoisoned due to __GFP_SKIP_KASAN, with the
 *    exception of pages unpoisoned by kasan_unpoison_vmalloc.
 * 3. The allocation was excluded from being checked due to sampling,
 *    see the call to kasan_unpoison_pages.
 *
 * Poisoning pages during deferred memory init will greatly lengthen the
 * process and cause problem in large memory systems as the deferred pages
 * initialization is done with interrupt disabled.
 *
 * Assuming that there will be no reference to those newly initialized
 * pages before they are ever allocated, this should have no effect on
 * KASAN memory tracking as the poison will be properly inserted at page
 * allocation time. The only corner case is when pages are allocated by
 * on-demand allocation and then freed again before the deferred pages
 * initialization is done, but this is not likely to happen.
 */
static inline bool should_skip_kasan_poison(struct page *page)
{
	if (IS_ENABLED(CONFIG_KASAN_GENERIC))
		return deferred_pages_enabled();

	return page_kasan_tag(page) == KASAN_TAG_KERNEL;
}

static void kernel_init_pages(struct page *page, int numpages)
{
	int i;

	/* s390's use of memset() could override KASAN redzones. */
	kasan_disable_current();
	for (i = 0; i < numpages; i++)
		clear_highpage_kasan_tagged(page + i);
	kasan_enable_current();
}

#ifdef CONFIG_MEM_ALLOC_PROFILING

/* Should be called only if mem_alloc_profiling_enabled() */
void __clear_page_tag_ref(struct page *page)
{
	union pgtag_ref_handle handle;
	union codetag_ref ref;

	if (get_page_tag_ref(page, &ref, &handle)) {
		set_codetag_empty(&ref);
		update_page_tag_ref(handle, &ref);
		put_page_tag_ref(handle);
	}
}

/* Should be called only if mem_alloc_profiling_enabled() */
static noinline
void __pgalloc_tag_add(struct page *page, struct task_struct *task,
		       unsigned int nr)
{
	union pgtag_ref_handle handle;
	union codetag_ref ref;

	if (get_page_tag_ref(page, &ref, &handle)) {
		alloc_tag_add(&ref, task->alloc_tag, PAGE_SIZE * nr);
		update_page_tag_ref(handle, &ref);
		put_page_tag_ref(handle);
	}
}

static inline void pgalloc_tag_add(struct page *page, struct task_struct *task,
				   unsigned int nr)
{
	if (mem_alloc_profiling_enabled())
		__pgalloc_tag_add(page, task, nr);
}

/* Should be called only if mem_alloc_profiling_enabled() */
static noinline
void __pgalloc_tag_sub(struct page *page, unsigned int nr)
{
	union pgtag_ref_handle handle;
	union codetag_ref ref;

	if (get_page_tag_ref(page, &ref, &handle)) {
		alloc_tag_sub(&ref, PAGE_SIZE * nr);
		update_page_tag_ref(handle, &ref);
		put_page_tag_ref(handle);
	}
}

static inline void pgalloc_tag_sub(struct page *page, unsigned int nr)
{
	if (mem_alloc_profiling_enabled())
		__pgalloc_tag_sub(page, nr);
}

/* When tag is not NULL, assuming mem_alloc_profiling_enabled */
static inline void pgalloc_tag_sub_pages(struct alloc_tag *tag, unsigned int nr)
{
	if (tag)
		this_cpu_sub(tag->counters->bytes, PAGE_SIZE * nr);
}

#else /* CONFIG_MEM_ALLOC_PROFILING */

static inline void pgalloc_tag_add(struct page *page, struct task_struct *task,
				   unsigned int nr) {}
static inline void pgalloc_tag_sub(struct page *page, unsigned int nr) {}
static inline void pgalloc_tag_sub_pages(struct alloc_tag *tag, unsigned int nr) {}

#endif /* CONFIG_MEM_ALLOC_PROFILING */

__always_inline bool __free_pages_prepare(struct page *page,
					  unsigned int order, fpi_t fpi_flags)
{
	int bad = 0;
	bool skip_kasan_poison = should_skip_kasan_poison(page);
	bool init = want_init_on_free();
	bool compound = PageCompound(page);
	struct folio *folio = page_folio(page);

	VM_BUG_ON_PAGE(PageTail(page), page);

	trace_mm_page_free(page, order);
	kmsan_free_page(page, order);

	if (memcg_kmem_online() && PageMemcgKmem(page))
		__memcg_kmem_uncharge_page(page, order);

	/*
	 * In rare cases, when truncation or holepunching raced with
	 * munlock after VM_LOCKED was cleared, Mlocked may still be
	 * found set here.  This does not indicate a problem, unless
	 * "unevictable_pgs_cleared" appears worryingly large.
	 */
	if (unlikely(folio_test_mlocked(folio))) {
		long nr_pages = folio_nr_pages(folio);

		__folio_clear_mlocked(folio);
		zone_stat_mod_folio(folio, NR_MLOCK, -nr_pages);
		count_vm_events(UNEVICTABLE_PGCLEARED, nr_pages);
	}

	if (unlikely(PageHWPoison(page)) && !order) {
		/* Do not let hwpoison pages hit pcplists/buddy */
		reset_page_owner(page, order);
		page_table_check_free(page, order);
		pgalloc_tag_sub(page, 1 << order);

		/*
		 * The page is isolated and accounted for.
		 * Mark the codetag as empty to avoid accounting error
		 * when the page is freed by unpoison_memory().
		 */
		clear_page_tag_ref(page);
		return false;
	}

	VM_BUG_ON_PAGE(compound && compound_order(page) != order, page);

	/*
	 * Check tail pages before head page information is cleared to
	 * avoid checking PageCompound for order-0 pages.
	 */
	if (unlikely(order)) {
		int i;

		if (compound) {
			page[1].flags.f &= ~PAGE_FLAGS_SECOND;
#ifdef NR_PAGES_IN_LARGE_FOLIO
			folio->_nr_pages = 0;
#endif
		}
		for (i = 1; i < (1 << order); i++) {
			if (compound)
				bad += free_tail_page_prepare(page, page + i);
			if (is_check_pages_enabled()) {
				if (free_page_is_bad(page + i)) {
					bad++;
					continue;
				}
			}
			(page + i)->flags.f &= ~PAGE_FLAGS_CHECK_AT_PREP;
		}
	}
	if (folio_test_anon(folio)) {
		mod_mthp_stat(order, MTHP_STAT_NR_ANON, -1);
		folio->mapping = NULL;
	}
	if (unlikely(page_has_type(page)))
		/* Reset the page_type (which overlays _mapcount) */
		page->page_type = UINT_MAX;

	if (is_check_pages_enabled()) {
		if (free_page_is_bad(page))
			bad++;
		if (bad)
			return false;
	}

	page_cpupid_reset_last(page);
	page->flags.f &= ~PAGE_FLAGS_CHECK_AT_PREP;
	page->private = 0;
	reset_page_owner(page, order);
	page_table_check_free(page, order);
	pgalloc_tag_sub(page, 1 << order);

	if (!PageHighMem(page) && !(fpi_flags & FPI_TRYLOCK)) {
		debug_check_no_locks_freed(page_address(page),
					   PAGE_SIZE << order);
		debug_check_no_obj_freed(page_address(page),
					   PAGE_SIZE << order);
	}

	kernel_poison_pages(page, 1 << order);

	/*
	 * As memory initialization might be integrated into KASAN,
	 * KASAN poisoning and memory initialization code must be
	 * kept together to avoid discrepancies in behavior.
	 *
	 * With hardware tag-based KASAN, memory tags must be set before the
	 * page becomes unavailable via debug_pagealloc or arch_free_page.
	 */
	if (!skip_kasan_poison) {
		kasan_poison_pages(page, order, init);

		/* Memory is already initialized if KASAN did it internally. */
		if (kasan_has_integrated_init())
			init = false;
	}
	if (init)
		kernel_init_pages(page, 1 << order);

	/*
	 * arch_free_page() can make the page's contents inaccessible.  s390
	 * does this.  So nothing which can access the page's contents should
	 * happen after this.
	 */
	arch_free_page(page, order);

	debug_pagealloc_unmap_pages(page, 1 << order);

	return true;
}

bool free_pages_prepare(struct page *page, unsigned int order)
{
	return __free_pages_prepare(page, order, FPI_NONE);
}

/*
 * Free PCP pages to zone buddy. First does a bottom-up merge pass
 * over PagePCPBuddy entries under pcp->lock only (already held by
 * caller). Only pages marked PagePCPBuddy (owned-block pages on
 * their home PCP) participate in merging; non-owned pages (Phase
 * 2/3 fragments) are skipped and drain individually.
 *
 * Then drains pages to zone under zone->lock, starting with
 * fully-merged pageblocks via round-robin. When those are exhausted,
 * falls through to smaller orders. Draining a pageblock-order page
 * disowns the block.
 */
static void free_pcppages_bulk(struct zone *zone, int count,
				struct per_cpu_pages *pcp)
{
	unsigned long flags;
	unsigned int order;
	struct page *page;
	int mt, pindex;

	/*
	 * Ensure proper count is passed which otherwise would stuck in the
	 * below while (list_empty(list)) loop.
	 */
	count = min(pcp->count, count);

	/* PCP merge pass */
	for (order = 0; order < pageblock_order; order++) {
		for (mt = 0; mt < MIGRATE_PCPTYPES; mt++) {
			struct list_head *list;
			struct page *page, *tmp;

			list = &pcp->lists[order_to_pindex(mt, order)];
			list_for_each_entry_safe(page, tmp, list, pcp_list) {
				unsigned long pfn = page_to_pfn(page);
				unsigned long buddy_pfn = __find_buddy_pfn(pfn, order);
				struct page *buddy = page + (buddy_pfn - pfn);
				unsigned long combined_pfn;
				struct page *combined;

				if (!PagePCPBuddy(page))
					continue;
				if (!PagePCPBuddy(buddy))
					continue;
				if (pcp_buddy_order(buddy) != order)
					continue;

				/* Don't corrupt the safe iterator! */
				if (buddy == tmp)
					tmp = list_next_entry(tmp, pcp_list);

				pcp_dequeue(pcp, page, order);
				pcp_dequeue(pcp, buddy, order);

				combined_pfn = buddy_pfn & pfn;
				combined = page + (combined_pfn - pfn);

				__SetPagePCPBuddy(combined);
				pcp_enqueue_tail(pcp, combined, mt, order + 1);
			}
		}
	}

	/* Ensure pageblock orders are drained first. */
	pindex = order_to_pindex(0, pageblock_order) - 1;

	spin_lock_irqsave(&zone->lock, flags);

	while (count > 0) {
		struct list_head *list;
		int nr_pages;

		/* Remove pages from lists in a round-robin fashion. */
		do {
			if (++pindex > NR_PCP_LISTS - 1)
				pindex = 0;
			list = &pcp->lists[pindex];
		} while (list_empty(list));

		order = pindex_to_order(pindex);
		nr_pages = 1 << order;
		do {
			fpi_t fpi = FPI_NONE;
			unsigned long pfn;

			page = list_last_entry(list, struct page, pcp_list);
			pfn = page_to_pfn(page);
			mt = get_pfnblock_migratetype(page, pfn);

			/*
			 * Owned fragment going to zone buddy: queue
			 * block for recovery during the next refill,
			 * and keep it away from other CPUs (tail).
			 */
			if (PagePCPBuddy(page) && order < pageblock_order) {
				struct pageblock_data *pbd;

				pbd = pfn_to_pageblock(page, pfn);
				if (list_empty(&pbd->cpu_node))
					list_add(&pbd->cpu_node, &pcp->owned_blocks);
				fpi = FPI_TO_TAIL;
			}

			pcp_dequeue(pcp, page, order);
			count -= nr_pages;

			__free_one_page(page, pfn, zone, order, mt, fpi);
			trace_mm_page_pcpu_drain(page, order, mt);
		} while (count > 0 && !list_empty(list));
	}

	spin_unlock_irqrestore(&zone->lock, flags);
}

/*
 * Search PCP free lists for a page of at least the requested order.
 * If found at a higher order, split and place remainders on PCP lists.
 * Returns NULL if nothing available on the PCP.
 */
static struct page *pcp_rmqueue_smallest(struct per_cpu_pages *pcp,
					 int migratetype, unsigned int order)
{
	unsigned int high;

	for (high = order; high <= pageblock_order; high++) {
		struct list_head *list;
		unsigned long size;
		struct page *page;
		bool owned;

		list = &pcp->lists[order_to_pindex(migratetype, high)];
		if (list_empty(list))
			continue;

		page = list_first_entry(list, struct page, pcp_list);
		/* Save before pcp_dequeue() clears it */
		owned = PagePCPBuddy(page);
		pcp_dequeue(pcp, page, high);

		size = 1 << high;
		while (high > order) {
			high--;
			size >>= 1;
			if (owned)
				__SetPagePCPBuddy(&page[size]);
			pcp_enqueue(pcp, &page[size], migratetype, high);
		}

		return page;
	}
	return NULL;
}

/* Split a multi-block free page into its individual pageblocks. */
static void split_large_buddy(struct zone *zone, struct page *page,
			      unsigned long pfn, int order, fpi_t fpi)
{
	unsigned long end = pfn + (1 << order);

	VM_WARN_ON_ONCE(!IS_ALIGNED(pfn, 1 << order));
	/* Caller removed page from freelist, buddy info cleared! */
	VM_WARN_ON_ONCE(PageBuddy(page));
	VM_WARN_ON_ONCE(PagePCPBuddy(page));

	if (order > pageblock_order)
		order = pageblock_order;

	do {
		int mt = get_pfnblock_migratetype(page, pfn);

		__free_one_page(page, pfn, zone, order, mt, fpi);
		pfn += 1 << order;
		if (pfn == end)
			break;
		page = pfn_to_page(pfn);
	} while (1);
}

static void add_page_to_zone_llist(struct zone *zone, struct page *page,
				   unsigned int order)
{
	/* Remember the order */
	page->private = order;
	/* Add the page to the free list */
	llist_add(&page->pcp_llist, &zone->trylock_free_pages);
}

static void free_one_page(struct zone *zone, struct page *page,
			  unsigned long pfn, unsigned int order,
			  fpi_t fpi_flags)
{
	struct llist_head *llhead;
	unsigned long flags;

	if (unlikely(fpi_flags & FPI_TRYLOCK)) {
		if (!spin_trylock_irqsave(&zone->lock, flags)) {
			add_page_to_zone_llist(zone, page, order);
			return;
		}
	} else {
		spin_lock_irqsave(&zone->lock, flags);
	}

	/* The lock succeeded. Process deferred pages. */
	llhead = &zone->trylock_free_pages;
	if (unlikely(!llist_empty(llhead) && !(fpi_flags & FPI_TRYLOCK))) {
		struct llist_node *llnode;
		struct page *p, *tmp;

		llnode = llist_del_all(llhead);
		llist_for_each_entry_safe(p, tmp, llnode, pcp_llist) {
			unsigned int p_order = p->private;

			split_large_buddy(zone, p, page_to_pfn(p), p_order, fpi_flags);
			__count_vm_events(PGFREE, 1 << p_order);
		}
	}
	split_large_buddy(zone, page, pfn, order, fpi_flags);
	spin_unlock_irqrestore(&zone->lock, flags);

	__count_vm_events(PGFREE, 1 << order);
}

static void __free_pages_ok(struct page *page, unsigned int order,
			    fpi_t fpi_flags)
{
	unsigned long pfn = page_to_pfn(page);
	struct zone *zone = page_zone(page);

	if (__free_pages_prepare(page, order, fpi_flags))
		free_one_page(zone, page, pfn, order, fpi_flags);
}

void __meminit __free_pages_core(struct page *page, unsigned int order,
		enum meminit_context context)
{
	unsigned int nr_pages = 1 << order;
	struct page *p = page;
	unsigned int loop;

	/*
	 * When initializing the memmap, __init_single_page() sets the refcount
	 * of all pages to 1 ("allocated"/"not free"). We have to set the
	 * refcount of all involved pages to 0.
	 *
	 * Note that hotplugged memory pages are initialized to PageOffline().
	 * Pages freed from memblock might be marked as reserved.
	 */
	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG) &&
	    unlikely(context == MEMINIT_HOTPLUG)) {
		for (loop = 0; loop < nr_pages; loop++, p++) {
			VM_WARN_ON_ONCE(PageReserved(p));
			__ClearPageOffline(p);
			set_page_count(p, 0);
		}

		adjust_managed_page_count(page, nr_pages);
	} else {
		for (loop = 0; loop < nr_pages; loop++, p++) {
			__ClearPageReserved(p);
			set_page_count(p, 0);
		}

		/* memblock adjusts totalram_pages() manually. */
		atomic_long_add(nr_pages, &page_zone(page)->managed_pages);
	}

	if (page_contains_unaccepted(page, order)) {
		if (order == MAX_PAGE_ORDER && __free_unaccepted(page))
			return;

		accept_memory(page_to_phys(page), PAGE_SIZE << order);
	}

	/*
	 * Bypass PCP and place fresh pages right to the tail, primarily
	 * relevant for memory onlining.
	 */
	__free_pages_ok(page, order, FPI_TO_TAIL);
}

/*
 * Check that the whole (or subset of) a pageblock given by the interval of
 * [start_pfn, end_pfn) is valid and within the same zone, before scanning it
 * with the migration of free compaction scanner.
 *
 * Return struct page pointer of start_pfn, or NULL if checks were not passed.
 *
 * It's possible on some configurations to have a setup like node0 node1 node0
 * i.e. it's possible that all pages within a zones range of pages do not
 * belong to a single zone. We assume that a border between node0 and node1
 * can occur within a single pageblock, but not a node0 node1 node0
 * interleaving within a single pageblock. It is therefore sufficient to check
 * the first and last page of a pageblock and avoid checking each individual
 * page in a pageblock.
 *
 * Note: the function may return non-NULL struct page even for a page block
 * which contains a memory hole (i.e. there is no physical memory for a subset
 * of the pfn range). For example, if the pageblock order is MAX_PAGE_ORDER, which
 * will fall into 2 sub-sections, and the end pfn of the pageblock may be hole
 * even though the start pfn is online and valid. This should be safe most of
 * the time because struct pages are still initialized via init_unavailable_range()
 * and pfn walkers shouldn't touch any physical memory range for which they do
 * not recognize any specific metadata in struct pages.
 */
struct page *__pageblock_pfn_to_page(unsigned long start_pfn,
				     unsigned long end_pfn, struct zone *zone)
{
	struct page *start_page;
	struct page *end_page;

	/* end_pfn is one past the range we are checking */
	end_pfn--;

	if (!pfn_valid(end_pfn))
		return NULL;

	start_page = pfn_to_online_page(start_pfn);
	if (!start_page)
		return NULL;

	if (page_zone(start_page) != zone)
		return NULL;

	end_page = pfn_to_page(end_pfn);

	/* This gives a shorter code than deriving page_zone(end_page) */
	if (page_zone_id(start_page) != page_zone_id(end_page))
		return NULL;

	return start_page;
}

/*
 * The order of subdivision here is critical for the IO subsystem.
 * Please do not alter this order without good reasons and regression
 * testing. Specifically, as large blocks of memory are subdivided,
 * the order in which smaller blocks are delivered depends on the order
 * they're subdivided in this function. This is the primary factor
 * influencing the order in which pages are delivered to the IO
 * subsystem according to empirical testing, and this is also justified
 * by considering the behavior of a buddy system containing a single
 * large block of memory acted on by a series of small allocations.
 * This behavior is a critical factor in sglist merging's success.
 *
 * -- nyc
 */
static inline unsigned int expand(struct zone *zone, struct page *page, int low,
				  int high, int migratetype)
{
	unsigned int size = 1 << high;
	unsigned int nr_added = 0;

	while (high > low) {
		high--;
		size >>= 1;
		VM_BUG_ON_PAGE(bad_range(zone, &page[size]), &page[size]);

		/*
		 * Mark as guard pages (or page), that will allow to
		 * merge back to allocator when buddy will be freed.
		 * Corresponding page table entries will not be touched,
		 * pages will stay not present in virtual address space
		 */
		if (set_page_guard(zone, &page[size], high))
			continue;

		__add_to_free_list(&page[size], zone, high, migratetype, false);
		set_buddy_order(&page[size], high);
		nr_added += size;
	}

	return nr_added;
}

static __always_inline void page_del_and_expand(struct zone *zone,
						struct page *page, int low,
						int high, int migratetype)
{
	int nr_pages = 1 << high;

	/*
	 * If we're splitting a page that spans at least a full pageblock,
	 * each constituent pageblock transitions from fully-free to in-use.
	 * Clear PB_all_free and update superpageblock accounting for ALL
	 * pageblocks in the range, not just the first one.
	 */
	if (high >= pageblock_order) {
		unsigned long pfn = page_to_pfn(page);
		unsigned long end_pfn = pfn + (1 << high);

		for (; pfn < end_pfn; pfn += pageblock_nr_pages) {
			struct page *pb_page = pfn_to_page(pfn);

			if (get_pfnblock_bit(pb_page, pfn, PB_all_free)) {
				clear_pfnblock_bit(pb_page, pfn, PB_all_free);
				superpageblock_pb_now_used(pb_page);
			}
			__spb_set_has_type(pb_page, migratetype);
		}
		/* Single list update after all pageblocks processed */
		{
			struct superpageblock *sb =
				pfn_to_superpageblock(zone,
						      page_to_pfn(page));
			if (sb)
				spb_update_list(sb);
		}
	} else {
		/*
		 * Sub-pageblock allocation: set PB_has_<migratetype> for
		 * the containing pageblock. Idempotent — only increments
		 * the counter on the first allocation of this type.
		 */
		__spb_set_has_type(page, migratetype);
		{
			struct superpageblock *sb =
				pfn_to_superpageblock(zone,
						      page_to_pfn(page));
			if (sb)
				spb_update_list(sb);
		}
	}

	__del_page_from_free_list(page, zone, high, migratetype);
	nr_pages -= expand(zone, page, low, high, migratetype);
	account_freepages(zone, -nr_pages, migratetype);
}

static void check_new_page_bad(struct page *page)
{
	if (unlikely(PageHWPoison(page))) {
		/* Don't complain about hwpoisoned pages */
		if (PageBuddy(page))
			__ClearPageBuddy(page);
		return;
	}

	bad_page(page,
		 page_bad_reason(page, PAGE_FLAGS_CHECK_AT_PREP));
}

/*
 * This page is about to be returned from the page allocator
 */
static bool check_new_page(struct page *page)
{
	if (likely(page_expected_state(page,
				PAGE_FLAGS_CHECK_AT_PREP|__PG_HWPOISON)))
		return false;

	check_new_page_bad(page);
	return true;
}

static inline bool check_new_pages(struct page *page, unsigned int order)
{
	if (is_check_pages_enabled()) {
		for (int i = 0; i < (1 << order); i++) {
			struct page *p = page + i;

			if (check_new_page(p))
				return true;
		}
	}

	return false;
}

static inline bool should_skip_kasan_unpoison(gfp_t flags)
{
	/* Don't skip if a software KASAN mode is enabled. */
	if (IS_ENABLED(CONFIG_KASAN_GENERIC) ||
	    IS_ENABLED(CONFIG_KASAN_SW_TAGS))
		return false;

	/* Skip, if hardware tag-based KASAN is not enabled. */
	if (!kasan_hw_tags_enabled())
		return true;

	/*
	 * With hardware tag-based KASAN enabled, skip if this has been
	 * requested via __GFP_SKIP_KASAN.
	 */
	return flags & __GFP_SKIP_KASAN;
}

static inline bool should_skip_init(gfp_t flags)
{
	/* Don't skip, if hardware tag-based KASAN is not enabled. */
	if (!kasan_hw_tags_enabled())
		return false;

	/* For hardware tag-based KASAN, skip if requested. */
	return (flags & __GFP_SKIP_ZERO);
}

inline void post_alloc_hook(struct page *page, unsigned int order,
				gfp_t gfp_flags)
{
	bool init = !want_init_on_free() && want_init_on_alloc(gfp_flags) &&
			!should_skip_init(gfp_flags);
	bool zero_tags = init && (gfp_flags & __GFP_ZEROTAGS);
	int i;

	set_page_private(page, 0);

	arch_alloc_page(page, order);
	debug_pagealloc_map_pages(page, 1 << order);

	/*
	 * Page unpoisoning must happen before memory initialization.
	 * Otherwise, the poison pattern will be overwritten for __GFP_ZERO
	 * allocations and the page unpoisoning code will complain.
	 */
	kernel_unpoison_pages(page, 1 << order);

	/*
	 * As memory initialization might be integrated into KASAN,
	 * KASAN unpoisoning and memory initialization code must be
	 * kept together to avoid discrepancies in behavior.
	 */

	/*
	 * If memory tags should be zeroed
	 * (which happens only when memory should be initialized as well).
	 */
	if (zero_tags)
		init = !tag_clear_highpages(page, 1 << order);

	if (!should_skip_kasan_unpoison(gfp_flags) &&
	    kasan_unpoison_pages(page, order, init)) {
		/* Take note that memory was initialized by KASAN. */
		if (kasan_has_integrated_init())
			init = false;
	} else {
		/*
		 * If memory tags have not been set by KASAN, reset the page
		 * tags to ensure page_address() dereferencing does not fault.
		 */
		for (i = 0; i != 1 << order; ++i)
			page_kasan_tag_reset(page + i);
	}
	/* If memory is still not initialized, initialize it now. */
	if (init)
		kernel_init_pages(page, 1 << order);

	set_page_owner(page, order, gfp_flags);
	page_table_check_alloc(page, order);
	pgalloc_tag_add(page, current, 1 << order);
}

static void prep_new_page(struct page *page, unsigned int order, gfp_t gfp_flags,
							unsigned int alloc_flags)
{
	post_alloc_hook(page, order, gfp_flags);

	if (order && (gfp_flags & __GFP_COMP))
		prep_compound_page(page, order);

	/*
	 * page is set pfmemalloc when ALLOC_NO_WATERMARKS was necessary to
	 * allocate the page. The expectation is that the caller is taking
	 * steps that will free more memory. The caller should avoid the page
	 * being used for !PFMEMALLOC purposes.
	 */
	if (alloc_flags & ALLOC_NO_WATERMARKS)
		set_page_pfmemalloc(page);
	else
		clear_page_pfmemalloc(page);
}

/* Bounded scan limit when searching free lists for tainted superpageblock pages */
#define SPB_SCAN_LIMIT 8

/*
 * Reserve free pageblocks in tainted superpageblocks for unmovable/reclaimable
 * allocations.  Movable allocations skip tainted superpageblocks that have
 * fewer than this many free pageblocks, ensuring that unmovable claims
 * always find room in existing tainted superpageblocks instead of spilling
 * into clean ones.
 *
 * Scale with SPB size: reserve ~3% of pageblocks (minimum 4).
 * For a 512-pageblock SPB this gives 16 reserved pageblocks.
 */
#define SPB_TAINTED_RESERVE_MIN	4

static inline u16 spb_tainted_reserve(const struct superpageblock *sb)
{
	return max_t(u16, SPB_TAINTED_RESERVE_MIN, sb->total_pageblocks / 32);
}

/*
 * High-water threshold for proactively kicking the slab shrinker. When a
 * non-movable allocation consumes from a tainted SPB whose total free
 * pages have fallen below spb_tainted_reserve worth of pages, queue a
 * shrink so we start freeing slab memory before the SPB is exhausted.
 *
 * Compared against nr_free_pages rather than nr_free (whole pageblocks):
 * sub-pageblock allocations and fragmented free space don't move the
 * pageblock count, but they do consume the SPB's freeable capacity, and
 * we can't assume slab reclaim will produce whole pageblocks either.
 */
static inline bool spb_below_shrink_high_water(const struct superpageblock *sb)
{
	return sb->nr_free_pages <
		(unsigned long)spb_tainted_reserve(sb) * pageblock_nr_pages;
}

/*
 * spb_react_to_tainted_alloc - kick reclaim machinery on a tainted-SPB alloc.
 *
 * Called from each PASS_1/2/2B/2C/2D success path after a successful
 * allocation against a tainted SPB. If the SPB is below its shrink
 * high-water mark, queue the SPB-driven slab shrink and try to start
 * the per-SPB defrag worker. Both have their own cooldown gates inside,
 * so this is cheap to call on every such allocation.
 *
 * Skips quickly when the SPB is not tainted (e.g. movable allocation
 * landing on a clean SPB) or when the high-water mark hasn't been
 * crossed.
 */
static inline void spb_react_to_tainted_alloc(struct superpageblock *sb,
					      struct zone *zone)
{
	if (spb_get_category(sb) != SB_TAINTED)
		return;
	if (!spb_below_shrink_high_water(sb))
		return;
	queue_spb_slab_shrink(zone);
	spb_maybe_start_defrag(sb);
}

/*
 * On systems with many superpageblocks, we can afford to "write off"
 * tainted superpageblocks by aggressively packing unmovable/reclaimable
 * allocations into them — even sub-pageblock fragments — to keep clean
 * superpageblocks clean for future 1GB hugepage and contiguous allocations.
 *
 * On small systems (few superpageblocks), each SPB represents a large
 * fraction of total memory. Aggressively claiming sub-pageblock movable
 * fragments from tainted SPBs would destroy MOVABLE capacity that the
 * system can't afford to lose, with little benefit since there are too
 * few SPBs to meaningfully separate movable from unmovable anyway.
 *
 * This threshold controls the crossover: above it, prefer concentrating
 * non-movable allocations in tainted SPBs at any granularity; below it,
 * only claim whole free pageblocks from tainted SPBs.
 */
#define SPB_AGGRESSIVE_THRESHOLD	8

/**
 * sb_preferred_for_movable - Find the fullest clean superpageblock for movable
 * @zone: zone to search
 *
 * Walk spb_lists[CLEAN] from nearly full toward emptiest — pack movable
 * allocations into already-partial superpageblocks before starting new ones.
 * Skip SB_FULL since those have no free pageblocks.
 * Returns NULL if no suitable superpageblock found.
 */
static struct superpageblock *sb_preferred_for_movable(struct zone *zone)
{
	int full;
	struct superpageblock *sb;

	for (full = SB_FULL_75; full < __NR_SB_FULLNESS; full++) {
		list_for_each_entry(sb, &zone->spb_lists[SB_CLEAN][full], list) {
			if (sb->nr_free)
				return sb;
		}
	}
	/* Fall back to empty superpageblocks — no clean partials available */
	return NULL;
}

/**
 * __rmqueue_from_sb - Try to allocate a page from a specific superpageblock
 * @zone: zone to allocate from
 * @order: allocation order
 * @migratetype: type to allocate
 * @sb: preferred superpageblock
 *
 * Scan the free list at the given order for a page within the superpageblock's
 * PFN range. Bounded scan to avoid excessive latency. Returns NULL if
 * no suitable page found.
 */
static struct page *__rmqueue_from_sb(struct zone *zone, unsigned int order,
				      int migratetype, struct superpageblock *sb)
{
	unsigned int current_order;
	struct free_area *area;
	struct page *page;

	/*
	 * Search the superpageblock's own free lists for all orders.
	 */
	for (current_order = order;
	     current_order < NR_PAGE_ORDERS;
	     ++current_order) {
		area = &sb->free_area[current_order];
		page = get_page_from_free_area(area, migratetype);
		if (!page)
			continue;

		page_del_and_expand(zone, page, order, current_order,
				    migratetype);
		return page;
	}

	return NULL;
}

/*
 * Go through the free lists for the given migratetype and remove
 * the smallest available page from the freelists.
 *
 * When superpageblocks are enabled, search per-superpageblock free lists first,
 * falling back to zone free lists for pages not in any superpageblock.
 */
static struct page *claim_whole_block(struct zone *zone, struct page *page,
		  int current_order, int order, int new_type, int old_type);
static struct page *try_to_claim_block(struct zone *zone, struct page *page,
		  int current_order, int order, int start_type,
		  int block_type, unsigned int alloc_flags,
		  bool from_tainted_spb);

/*
 * Snapshot of tainted-SPB state observed while __rmqueue_smallest walks the
 * free lists. Lets the caller (currently __rmqueue) decide whether to refuse
 * a fragmenting fallback when an existing tainted SPB could absorb the demand
 * once it is evacuated.
 */
struct spb_tainted_walk {
	bool saw_free_pages;	/* tainted SPB has any free pages, any order */
	bool saw_free_pb;	/* tainted SPB has at least one free pageblock */
	bool saw_below_reserve;	/* tainted SPB has nr_free <= spb_tainted_reserve */
};

/*
 * Stage 5 PASS_1 fast-path hint: most-recent SPB this CPU successfully
 * allocated from for a given (zone, order, migratetype). Combined with
 * the per-zone zone->sb_hint[][], this lets PASS_1 skip the linear walk
 * of spb_lists[cat][full] in the common case (~78 SPBs visited per
 * order-0 MOVABLE alloc on the build-403 baseline). Stale hints are
 * harmless — the try-alloc just falls through to the standard list walk
 * on miss.
 *
 * The slot stores both the zone pointer and the SPB pointer because
 * zone_idx(zone) is per-pgdat (not globally unique on NUMA), so two
 * nodes' ZONE_NORMAL share the same array index. The zone-pointer check
 * on read prevents a cross-node SPB from being handed back to the wrong
 * zone (which would corrupt per-zone NR_FREE_PAGES accounting).
 */
struct spb_warm_hint_slot {
	struct zone		*zone;
	struct superpageblock	*sb;
};
struct spb_warm_hints {
	struct spb_warm_hint_slot slot[MAX_NR_ZONES][NR_PAGE_ORDERS][MIGRATE_PCPTYPES];
};
static DEFINE_PER_CPU(struct spb_warm_hints, spb_warm_hints);

/**
 * spb_invalidate_warm_hints - drop all cached hints into @zone
 * @zone: zone whose SPB array is about to change
 *
 * Called from memory hotplug paths that resize zone->superpageblocks
 * (and therefore invalidate every SPB pointer for @zone). Must be
 * called with zone->lock held; the lock serializes against any CPU
 * doing a hint read inside __rmqueue_smallest (also under zone->lock),
 * so callers see either pre-invalidation state (old SPB pointers,
 * still-valid old array) or post-invalidation state (NULL slots) —
 * never a half-state with stale pointers into a freed array.
 */
void spb_invalidate_warm_hints(struct zone *zone)
{
	enum zone_type zidx = zone_idx(zone);
	int cpu, order, mt;

	lockdep_assert_held(&zone->lock);

	memset(zone->sb_hint, 0, sizeof(zone->sb_hint));

	for_each_possible_cpu(cpu) {
		struct spb_warm_hints *h = per_cpu_ptr(&spb_warm_hints, cpu);

		for (order = 0; order < NR_PAGE_ORDERS; order++) {
			for (mt = 0; mt < MIGRATE_PCPTYPES; mt++) {
				if (h->slot[zidx][order][mt].zone != zone)
					continue;
				h->slot[zidx][order][mt].zone = NULL;
				h->slot[zidx][order][mt].sb = NULL;
			}
		}
	}
}

/*
 * Try to allocate from a single SPB using PASS_1 semantics:
 * whole pageblock first (PCP-buddy friendly), then sub-pageblock.
 * Returns the page on success, NULL on miss. Caller is responsible
 * for tracepoints, hint updates, and shrinker queueing.
 */
static struct page *try_alloc_from_sb_pass1(struct zone *zone,
					    struct superpageblock *sb,
					    unsigned int order,
					    int migratetype)
{
	unsigned int current_order;
	struct free_area *area;
	struct page *page;

	if (!sb->nr_free_pages)
		return NULL;

	for (current_order = max(order, pageblock_order);
	     current_order < NR_PAGE_ORDERS;
	     ++current_order) {
		area = &sb->free_area[current_order];
		page = get_page_from_free_area(area, migratetype);
		if (!page)
			continue;
		page_del_and_expand(zone, page, order,
				    current_order, migratetype);
		return page;
	}
	if (order < pageblock_order) {
		for (current_order = order;
		     current_order < pageblock_order;
		     ++current_order) {
			area = &sb->free_area[current_order];
			page = get_page_from_free_area(area, migratetype);
			if (!page)
				continue;
			page_del_and_expand(zone, page, order,
					    current_order, migratetype);
			return page;
		}
	}
	return NULL;
}

static __always_inline
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
				int migratetype, unsigned int alloc_flags,
				struct spb_tainted_walk *walk)
{
	unsigned int current_order;
	struct free_area *area;
	struct page *page;
	int full;
	struct superpageblock *sb;
	int opposite_mt;
	/*
	 * Diagnostic counter for the spb_alloc_walk tracepoint. Counts how
	 * many SPBs were visited (across all Passes) before this allocation
	 * succeeded or fell through. Used to characterize the cost of the
	 * linear spb_lists walk and identify pathological cases.
	 */
	unsigned int n_spbs_visited = 0;

#define SPB_WALK_DONE(_outcome) \
	trace_spb_alloc_walk(zone, order, migratetype, alloc_flags, \
			     (_outcome), n_spbs_visited)
	/*
	 * Category search order: 2 passes.
	 * Movable: clean first, then tainted (pack into clean SBs).
	 * Others: tainted first, then clean (concentrate in tainted SBs).
	 */
	static const enum sb_category cat_order[2][2] = {
		[0] = { SB_TAINTED, SB_CLEAN },  /* unmovable/reclaimable */
		[1] = { SB_CLEAN, SB_TAINTED },  /* movable */
	};
	int movable = (migratetype == MIGRATE_MOVABLE) ? 1 : 0;

	/*
	 * Stage 5 PASS_1 fast-path: try per-CPU then per-zone hint SPB
	 * before the linear list walk. The hint stores the SPB that last
	 * satisfied a PASS_1 alloc for this (zone, order, migratetype).
	 * On hit, we skip the entire spb_lists walk (n_spbs_visited stays
	 * 0, which shows up as the [0,5) bucket in the spb_alloc_walk
	 * tracepoint histogram). Skip for HIGHATOMIC/CMA/ISOLATE — those
	 * paths are already cheap (atomic-NORETRY skip) or rare.
	 */
	if (migratetype < MIGRATE_PCPTYPES) {
		enum zone_type zidx = zone_idx(zone);
		struct superpageblock *cpu_hint = NULL, *zone_hint;
		struct spb_warm_hint_slot *slot;

		slot = this_cpu_ptr(
			&spb_warm_hints.slot[zidx][order][migratetype]);
		/*
		 * Validate slot->zone == zone: zone_idx is per-pgdat, so
		 * on NUMA the same slot index is shared by every node's
		 * zone of this type. Without this check, a hint written
		 * from one node would be returned to allocations on
		 * another node and corrupt the wrong zone's accounting.
		 */
		if (slot->zone == zone)
			cpu_hint = slot->sb;
		if (cpu_hint) {
			page = try_alloc_from_sb_pass1(zone, cpu_hint,
						       order, migratetype);
			if (page) {
				spb_react_to_tainted_alloc(cpu_hint, zone);
				trace_mm_page_alloc_zone_locked(page, order,
				    migratetype,
				    pcp_allowed_order(order) &&
				    migratetype < MIGRATE_PCPTYPES);
				SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_1);
				return page;
			}
		}
		zone_hint = zone->sb_hint[order][migratetype];
		if (zone_hint && zone_hint != cpu_hint) {
			page = try_alloc_from_sb_pass1(zone, zone_hint,
						       order, migratetype);
			if (page) {
				spb_react_to_tainted_alloc(zone_hint, zone);
				slot->zone = zone;
				slot->sb = zone_hint;
				trace_mm_page_alloc_zone_locked(page, order,
				    migratetype,
				    pcp_allowed_order(order) &&
				    migratetype < MIGRATE_PCPTYPES);
				SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_1);
				return page;
			}
		}
	}

	/*
	 * Search per-superpageblock free lists for pages of the requested
	 * migratetype, walking superpageblocks from fullest to emptiest
	 * to pack allocations.
	 *
	 * For unmovable/reclaimable, prefer tainted superpageblocks to
	 * concentrate non-movable allocations into fewer superpageblocks.
	 * For movable, prefer clean superpageblocks to keep them homogeneous.
	 *
	 * Prefer whole pageblock allocations (>= pageblock_order) over
	 * sub-pageblock allocations because whole pageblocks enable the
	 * PCP buddy optimization for fast subsequent allocations.
	 *
	 * Search order:
	 * 1. Preferred SPBs: whole pageblock first, then sub-pageblock
	 * 2. Whole pageblock inline claim from tainted SPBs (non-movable only)
	 * 3. Whole pageblock from empty SPBs
	 * 4. Fallback to non-preferred SPBs
	 *
	 * Pass 1 tries whole pageblock first for PCP buddy optimization,
	 * then falls back to sub-pageblock within the same preferred SPBs.
	 * This ensures we never taint empty/clean SPBs while preferred
	 * SPBs still have free pages at any order.
	 */

	/* Pass 1: preferred SPBs — whole pageblock first, then sub-pageblock */
	for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
		enum sb_category cat = cat_order[movable][0];

		list_for_each_entry(sb,
			&zone->spb_lists[cat][full], list) {
			n_spbs_visited++;
			/*
			 * Snapshot tainted-SPB capacity before the
			 * nr_free_pages skip: an SPB with a free pageblock
			 * but nothing on the requested-MT freelist still
			 * counts as "could absorb this allocation after evac".
			 */
			if (walk && cat == SB_TAINTED) {
				if (sb->nr_free_pages)
					walk->saw_free_pages = true;
				if (sb->nr_free)
					walk->saw_free_pb = true;
				if (sb->nr_free <= spb_tainted_reserve(sb))
					walk->saw_below_reserve = true;
			}
			if (!sb->nr_free_pages)
				continue;
			/* Try whole pageblock (or larger) first for PCP buddy */
			for (current_order = max(order, pageblock_order);
			     current_order < NR_PAGE_ORDERS;
			     ++current_order) {
				area = &sb->free_area[current_order];
				page = get_page_from_free_area(
					area, migratetype);
				if (!page)
					continue;
				page_del_and_expand(zone, page,
					order, current_order,
					migratetype);
				if (cat == SB_TAINTED)
					spb_react_to_tainted_alloc(sb, zone);
				trace_mm_page_alloc_zone_locked(
					page, order, migratetype,
					pcp_allowed_order(order) &&
					migratetype < MIGRATE_PCPTYPES);
				SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_1);
				if (migratetype < MIGRATE_PCPTYPES) {
					struct spb_warm_hint_slot *slot;

					zone->sb_hint[order][migratetype] = sb;
					slot = this_cpu_ptr(&spb_warm_hints.slot
					    [zone_idx(zone)][order][migratetype]);
					slot->zone = zone;
					slot->sb = sb;
				}
				return page;
			}
			/* Then try sub-pageblock (no PCP buddy) */
			if (order < pageblock_order) {
				for (current_order = order;
				     current_order < pageblock_order;
				     ++current_order) {
					area = &sb->free_area[current_order];
					page = get_page_from_free_area(
						area, migratetype);
					if (!page)
						continue;
					page_del_and_expand(zone, page,
						order, current_order,
						migratetype);
					if (cat == SB_TAINTED)
						spb_react_to_tainted_alloc(sb, zone);
					trace_mm_page_alloc_zone_locked(
						page, order, migratetype,
						pcp_allowed_order(order) &&
						migratetype < MIGRATE_PCPTYPES);
					SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_1);
					if (migratetype < MIGRATE_PCPTYPES) {
						struct spb_warm_hint_slot *slot;

						zone->sb_hint[order][migratetype] = sb;
						slot = this_cpu_ptr(&spb_warm_hints.slot
						    [zone_idx(zone)][order][migratetype]);
						slot->zone = zone;
						slot->sb = sb;
					}
					return page;
				}
			}
		}
	}

	/*
	 * Pass 2: for non-movable allocations, try to claim free pageblocks
	 * from tainted superpageblocks. Free pageblocks in tainted SBs have
	 * pages on the MOVABLE free list (reset by mark_pageblock_free), so
	 * pass 1 misses them. Claim them inline to keep non-movable allocations
	 * concentrated in already-tainted superpageblocks.
	 *
	 * Try whole pageblock orders first (preferred for PCP buddy optimization),
	 * then fall back to sub-pageblock orders. Sub-pageblock claiming uses
	 * try_to_claim_block which checks whether the pageblock has enough
	 * compatible pages to justify claiming it.
	 */
	if (!movable && !is_migrate_cma(migratetype)) {
		for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
			list_for_each_entry(sb,
				&zone->spb_lists[SB_TAINTED][full], list) {
				n_spbs_visited++;
				if (!sb->nr_free)
					continue;
				for (current_order = max_t(unsigned int,
						order, pageblock_order);
				     current_order < NR_PAGE_ORDERS;
				     ++current_order) {
					area = &sb->free_area[current_order];
					page = get_page_from_free_area(
						area, MIGRATE_MOVABLE);
					if (!page)
						continue;
					if (get_pageblock_isolate(page))
						continue;
					if (is_migrate_cma(
					    get_pageblock_migratetype(page)))
						continue;
					page = claim_whole_block(zone, page,
						current_order, order,
						migratetype, MIGRATE_MOVABLE);
					spb_react_to_tainted_alloc(sb, zone);
					trace_mm_page_alloc_zone_locked(
						page, order, migratetype,
						pcp_allowed_order(order) &&
						migratetype < MIGRATE_PCPTYPES);
					SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_2);
					return page;
				}
			}
		}
		/* Pass 2b: sub-pageblock orders in tainted SPBs */
		for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
			list_for_each_entry(sb,
				&zone->spb_lists[SB_TAINTED][full], list) {
				int co;

				n_spbs_visited++;
				if (!sb->nr_free_pages)
					continue;
				for (co = min_t(int, pageblock_order - 1,
						NR_PAGE_ORDERS - 1);
				     co >= (int)order;
				     --co) {
					current_order = co;
					area = &sb->free_area[current_order];
					page = get_page_from_free_area(
						area, MIGRATE_MOVABLE);
					if (!page)
						continue;
					if (get_pageblock_isolate(page))
						continue;
					if (is_migrate_cma(
					    get_pageblock_migratetype(page)))
						continue;
					page = try_to_claim_block(zone, page,
						current_order, order,
						migratetype, MIGRATE_MOVABLE,
						0, true);
					if (!page)
						continue;
					spb_react_to_tainted_alloc(sb, zone);
					trace_mm_page_alloc_zone_locked(
						page, order, migratetype,
						pcp_allowed_order(order) &&
						migratetype < MIGRATE_PCPTYPES);
					SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_2B);
					return page;
				}
			}
		}

		/*
		 * Pass 2c: cross-non-movable borrow within tainted SPBs.
		 *
		 * If we're a non-movable alloc and Pass 1/2/2b couldn't find a
		 * buddy on our migratetype's free list anywhere, but tainted
		 * SPBs have free buddies on the *opposite* non-movable type's
		 * free list, take one of those.
		 *
		 * Why this happens: when pages are freed, __free_one_page puts
		 * them on the free_list determined by their pageblock's tag,
		 * not the original allocation's migratetype. Slab caches tend
		 * to be dense (RECL pageblocks fill up; few sub-PB fragments),
		 * while UNMOV pageblocks accumulate sparse free space from
		 * vmalloc/raw alloc churn. Net effect: tainted SPBs frequently
		 * have tens of thousands of free pages all on the UNMOV list,
		 * invisible to RECL allocs (or vice versa). Without this pass,
		 * the alloc falls through to Pass 3 and taints a fresh clean
		 * SPB even though the existing tainted ones have plenty of
		 * unused space.
		 *
		 * We do NOT relabel the source pageblock. The buddy is taken
		 * from @opposite_mt's free list and the splits go back on
		 * @opposite_mt's list (page_del_and_expand uses the same mt
		 * for delete and expand). The pageblock tag is unchanged, so
		 * the page returns to @opposite_mt's list when freed via
		 * __free_one_page. Effectively a borrow: the alloc takes a
		 * physical page from a UNMOV-tagged pageblock for a RECL
		 * use, and the page cycles back to UNMOV's list on free.
		 *
		 * We do set PB_has_<migratetype> via __spb_set_has_type so
		 * spb_defrag accounting reflects that this pageblock now hosts
		 * our migratetype's content too. PB_has_<opposite_mt> stays
		 * set since other buddies of that type remain.
		 *
		 * Restricted to UNMOV ↔ RECL. Movable allocations don't
		 * participate (they have their own Pass 4 fallback path).
		 *
		 * Restricted to SB_TAINTED to avoid spreading mixing into
		 * clean SPBs.
		 */
		opposite_mt = -1;
		if (migratetype == MIGRATE_UNMOVABLE)
			opposite_mt = MIGRATE_RECLAIMABLE;
		else if (migratetype == MIGRATE_RECLAIMABLE)
			opposite_mt = MIGRATE_UNMOVABLE;

		if (opposite_mt >= 0) {
			for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
				list_for_each_entry(sb,
					&zone->spb_lists[SB_TAINTED][full], list) {
					int co;

					n_spbs_visited++;
					if (!sb->nr_free_pages)
						continue;
					for (co = min_t(int, pageblock_order - 1,
							NR_PAGE_ORDERS - 1);
					     co >= (int)order;
					     --co) {
						current_order = co;
						area = &sb->free_area[current_order];
						page = get_page_from_free_area(
							area, opposite_mt);
						if (!page)
							continue;
						if (get_pageblock_isolate(page))
							continue;
						if (is_migrate_cma(
						    get_pageblock_migratetype(page)))
							continue;
						page_del_and_expand(zone, page,
							order, current_order,
							opposite_mt);
						__spb_set_has_type(page,
							migratetype);
						spb_react_to_tainted_alloc(sb, zone);
						trace_mm_page_alloc_zone_locked(
							page, order, migratetype,
							pcp_allowed_order(order) &&
							migratetype < MIGRATE_PCPTYPES);
						SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_2C);
						return page;
					}
				}
			}
		}

		/*
		 * Pass 2d: cross-MOV borrow within tainted SPBs.
		 *
		 * If Pass 1/2/2b/2c all failed, the next step is Pass 3
		 * which would taint a fresh clean SPB. Before that, try
		 * to borrow an individual buddy from a tainted SPB's
		 * MIGRATE_MOVABLE free list.
		 *
		 * Tainted SPBs accumulate large amounts of free space on
		 * the MOV free list (e.g. reclaimed page-cache pages
		 * whose pageblock tag is MOVABLE). Pass 1 cannot see
		 * those for non-movable allocs, Pass 2/2b cannot claim a
		 * whole pageblock when sb->nr_free == 0, and Pass 2c is
		 * restricted to UNMOV<->RECL. The result is a tainted
		 * SPB with tens to hundreds of thousands of free pages
		 * all unreachable from non-movable demand.
		 *
		 * Borrow semantics mirror Pass 2c: take a buddy from the
		 * MOVABLE free list without relabeling the source
		 * pageblock. The page is used for the requesting non-
		 * movable mt for the lifetime of the allocation, then on
		 * free returns to the MOVABLE list.
		 *
		 * Cost: the borrowed UNMOV/RECL content blocks
		 * compaction of its source pageblock until freed.
		 * Restricted to SB_TAINTED so the contamination is
		 * bounded to an already-tainted SPB; the alternative
		 * (Pass 3) taints a fresh clean SPB and removes a 1 GiB
		 * region from the clean pool, which is strictly worse.
		 *
		 * Skipped for movable allocs (they have Pass 4) and for
		 * CMA allocs.
		 */
		if (!movable && !is_migrate_cma(migratetype)) {
			for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
				list_for_each_entry(sb,
					&zone->spb_lists[SB_TAINTED][full], list) {
					int co;

					n_spbs_visited++;
					if (!sb->nr_free_pages)
						continue;
					for (co = min_t(int, pageblock_order - 1,
							NR_PAGE_ORDERS - 1);
					     co >= (int)order;
					     --co) {
						current_order = co;
						area = &sb->free_area[current_order];
						page = get_page_from_free_area(
							area, MIGRATE_MOVABLE);
						if (!page)
							continue;
						if (get_pageblock_isolate(page))
							continue;
						if (is_migrate_cma(
						    get_pageblock_migratetype(page)))
							continue;
						page_del_and_expand(zone, page,
							order, current_order,
							MIGRATE_MOVABLE);
						__spb_set_has_type(page,
							migratetype);
						spb_react_to_tainted_alloc(sb, zone);
						trace_mm_page_alloc_zone_locked(
							page, order, migratetype,
							pcp_allowed_order(order) &&
							migratetype < MIGRATE_PCPTYPES);
						SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_2D);
						return page;
					}
				}
			}
		}
	}

	/*
	 * About to fall through to Pass 3 (empty SPBs) or Pass 4 fallback,
	 * which risks tainting a clean SPB. If the tainted-SPB walk above
	 * showed that some tainted SPB is below its reserve threshold of
	 * free pageblocks, kick deferred evacuation so future allocations
	 * have a movable-evicted home in an already-tainted SPB.
	 *
	 * Queue slab shrink alongside evacuation: even when movable evac
	 * succeeds, shrinking slab in parallel keeps headroom available
	 * for the next burst, when the movable supply may have run out.
	 */
	if (walk && walk->saw_below_reserve) {
		queue_spb_evacuate(zone, order, migratetype);
		queue_spb_slab_shrink(zone);
	}

	/*
	 * Last-chance defrag trigger before tainting a fresh clean SPB.
	 * Walk the tainted-SPB list and try to wake the per-SPB defrag
	 * worker on each. Catches SPBs that are stuck in expired-cooldown
	 * state because no allocator activity has touched them recently
	 * (the routine event-driven trigger from spb_update_list only
	 * fires on bucket transitions, not on every alloc). Once the
	 * cooldown has expired, spb_maybe_start_defrag() will requeue
	 * work; otherwise the gate inside spb_needs_defrag() no-ops
	 * cheaply. Bounded by nr_tainted_spbs and only runs when we are
	 * already on the slow path of fragmenting the clean pool.
	 */
	for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
		list_for_each_entry(sb,
			&zone->spb_lists[SB_TAINTED][full], list) {
			spb_maybe_start_defrag(sb);
		}
	}

	/*
	 * Diagnostic: capture per-fall-through state so we can answer
	 * "why didn't an existing tainted SPB absorb this allocation?".
	 * The count loop walks the tainted-SPB lists looking for any SPB
	 * with a free buddy at the requested (order, migratetype). >0
	 * means buddies were available — Pass 1 missed them. 0 means
	 * the tainted pool genuinely had nothing usable. Loop is bounded
	 * by the number of tainted SPBs and runs only on the slow path
	 * (this is the fall-through to Pass 3/Pass 4). Skipped if the
	 * tracepoint is not active so there is zero cost in production.
	 */
	if (walk && trace_spb_alloc_fall_through_enabled()) {
		unsigned int n_tainted = 0, n_with_buddy = 0;

		for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
			list_for_each_entry(sb,
				&zone->spb_lists[SB_TAINTED][full], list) {
				n_tainted++;
				if (!list_empty(
				    &sb->free_area[order].free_list[migratetype]))
					n_with_buddy++;
			}
		}
		trace_spb_alloc_fall_through(zone, order, migratetype,
					     alloc_flags,
					     n_tainted, n_with_buddy,
					     walk->saw_free_pages,
					     walk->saw_free_pb,
					     walk->saw_below_reserve);
	}

	/* Pass 3: whole pageblock from empty superpageblocks */
	list_for_each_entry(sb, &zone->spb_empty, list) {
		n_spbs_visited++;
		if (!sb->nr_free_pages)
			continue;
		for (current_order = max(order, pageblock_order);
		     current_order < NR_PAGE_ORDERS;
		     ++current_order) {
			area = &sb->free_area[current_order];
			page = get_page_from_free_area(area, migratetype);
			if (!page)
				continue;
			page_del_and_expand(zone, page, order,
				current_order, migratetype);
			trace_mm_page_alloc_zone_locked(page, order,
				migratetype,
				pcp_allowed_order(order) &&
				migratetype < MIGRATE_PCPTYPES);
			SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_3);
			return page;
		}
	}

	/*
	 * Pass 4: movable allocations fall back to tainted SPBs.
	 * Non-movable allocations must NOT search clean SPBs here;
	 * stale migratetype labels create phantom non-movable free
	 * pages in clean SPBs that would cause unnecessary tainting.
	 * Let __rmqueue_claim and __rmqueue_steal handle non-movable
	 * fallback with proper ALLOC_NOFRAGMENT protection.
	 */
	if (movable) {
		for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
			enum sb_category cat = cat_order[movable][1];

			list_for_each_entry(sb,
				&zone->spb_lists[cat][full], list) {
				n_spbs_visited++;
				if (!sb->nr_free_pages)
					continue;
				/*
				 * Movable falling back to tainted: skip SBs
				 * with few free pageblocks to reserve space
				 * for future unmovable/reclaimable claims.
				 */
				if (sb->nr_free <= spb_tainted_reserve(sb))
					continue;
				for (current_order = order;
				     current_order < NR_PAGE_ORDERS;
				     ++current_order) {
					area = &sb->free_area[current_order];
					page = get_page_from_free_area(
						area, migratetype);
					if (!page)
						continue;
					page_del_and_expand(zone, page,
						order, current_order,
						migratetype);
					trace_mm_page_alloc_zone_locked(
						page, order, migratetype,
						pcp_allowed_order(order) &&
						migratetype < MIGRATE_PCPTYPES);
					SPB_WALK_DONE(SPB_ALLOC_OUTCOME_PASS_4);
					return page;
				}
			}
		}
	}

	/*
	 * Zone free lists: all pages should be on superpageblock lists.
	 * Finding a page here means zone hotplug added memory without
	 * setting up superpageblocks for the new range.
	 */
	for (current_order = order;
	     current_order < NR_PAGE_ORDERS; ++current_order) {
		area = &(zone->free_area[current_order]);
		page = get_page_from_free_area(area, migratetype);
		if (!page)
			continue;

		WARN_ON_ONCE(zone->superpageblocks);
		page_del_and_expand(zone, page, order, current_order,
				    migratetype);
		trace_mm_page_alloc_zone_locked(page, order, migratetype,
				pcp_allowed_order(order) &&
				migratetype < MIGRATE_PCPTYPES);
		SPB_WALK_DONE(SPB_ALLOC_OUTCOME_ZONE_FALLBACK);
		return page;
	}

	SPB_WALK_DONE(SPB_ALLOC_OUTCOME_NO_PAGE);
	return NULL;
#undef SPB_WALK_DONE
}


/*
 * This array describes the order lists are fallen back to when
 * the free lists for the desirable migrate type are depleted
 *
 * The other migratetypes do not have fallbacks.
 */
static int fallbacks[MIGRATE_PCPTYPES][MIGRATE_PCPTYPES - 1] = {
	[MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE   },
	[MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE },
	[MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE   },
};

/*
 * UNMOVABLE and RECLAIMABLE allocations should not share the same
 * pageblock. Their free pages are interchangeable on the buddy free
 * lists (alike_pages == 0 between them), so once a PB holds both
 * types the buddy can no longer tell them apart and any sticky
 * UNMOVABLE pinhole prevents the RECLAIMABLE pages from coalescing
 * into useful higher-order chunks when they drain back. MOVABLE may
 * mix with either, since MOVABLE pages can be migrated out.
 */
static inline bool noncompatible_cross_type(int start_type, int fallback_type)
{
	return (start_type == MIGRATE_UNMOVABLE &&
		fallback_type == MIGRATE_RECLAIMABLE) ||
	       (start_type == MIGRATE_RECLAIMABLE &&
		fallback_type == MIGRATE_UNMOVABLE);
}

#ifdef CONFIG_CMA
static __always_inline struct page *__rmqueue_cma_fallback(struct zone *zone,
					unsigned int order)
{
	return __rmqueue_smallest(zone, order, MIGRATE_CMA, 0, NULL);
}
#else
static inline struct page *__rmqueue_cma_fallback(struct zone *zone,
					unsigned int order) { return NULL; }
#endif

/*
 * Move all free pages of a block to new type's freelist. Caller needs to
 * change the block type.
 */
static int __move_freepages_block(struct zone *zone, unsigned long start_pfn,
				  int old_mt, int new_mt)
{
	struct page *page;
	unsigned long pfn, end_pfn;
	unsigned int order;
	int pages_moved = 0;

	VM_WARN_ON(start_pfn & (pageblock_nr_pages - 1));
	end_pfn = pageblock_end_pfn(start_pfn);

	for (pfn = start_pfn; pfn < end_pfn;) {
		page = pfn_to_page(pfn);
		if (!PageBuddy(page)) {
			pfn++;
			continue;
		}

		/* Make sure we are not inadvertently changing nodes */
		VM_BUG_ON_PAGE(page_to_nid(page) != zone_to_nid(zone), page);
		VM_BUG_ON_PAGE(page_zone(page) != zone, page);

		order = buddy_order(page);

		move_to_free_list(page, zone, order, old_mt, new_mt);

		pfn += 1 << order;
		pages_moved += 1 << order;
	}

	return pages_moved;
}

static bool prep_move_freepages_block(struct zone *zone, struct page *page,
				      unsigned long *start_pfn,
				      int *num_free, int *num_movable)
{
	unsigned long pfn, start, end;

	pfn = page_to_pfn(page);
	start = pageblock_start_pfn(pfn);
	end = pageblock_end_pfn(pfn);

	/*
	 * The caller only has the lock for @zone, don't touch ranges
	 * that straddle into other zones. While we could move part of
	 * the range that's inside the zone, this call is usually
	 * accompanied by other operations such as migratetype updates
	 * which also should be locked.
	 */
	if (!zone_spans_pfn(zone, start))
		return false;
	if (!zone_spans_pfn(zone, end - 1))
		return false;

	*start_pfn = start;

	if (num_free) {
		*num_free = 0;
		*num_movable = 0;
		for (pfn = start; pfn < end;) {
			page = pfn_to_page(pfn);
			if (PageBuddy(page)) {
				int nr = 1 << buddy_order(page);

				*num_free += nr;
				pfn += nr;
				continue;
			}
			/*
			 * We assume that pages that could be isolated for
			 * migration are movable. But we don't actually try
			 * isolating, as that would be expensive.
			 */
			if (PageLRU(page) || page_has_movable_ops(page))
				(*num_movable)++;
			pfn++;
		}
	}

	return true;
}

static int move_freepages_block(struct zone *zone, struct page *page,
				int old_mt, int new_mt)
{
	unsigned long start_pfn;
	int res;

	if (!prep_move_freepages_block(zone, page, &start_pfn, NULL, NULL))
		return -1;

	res = __move_freepages_block(zone, start_pfn, old_mt, new_mt);
	set_pageblock_migratetype(pfn_to_page(start_pfn), new_mt);

	return res;

}

#ifdef CONFIG_MEMORY_ISOLATION
/* Look for a buddy that straddles start_pfn */
static unsigned long find_large_buddy(unsigned long start_pfn)
{
	/*
	 * If start_pfn is not an order-0 PageBuddy, next PageBuddy containing
	 * start_pfn has minimal order of __ffs(start_pfn) + 1. Start checking
	 * the order with __ffs(start_pfn). If start_pfn is order-0 PageBuddy,
	 * the starting order does not matter.
	 */
	int order = start_pfn ? __ffs(start_pfn) : MAX_PAGE_ORDER;
	struct page *page;
	unsigned long pfn = start_pfn;

	while (!PageBuddy(page = pfn_to_page(pfn))) {
		/* Nothing found */
		if (++order > MAX_PAGE_ORDER)
			return start_pfn;
		pfn &= ~0UL << order;
	}

	/*
	 * Found a preceding buddy, but does it straddle?
	 */
	if (pfn + (1 << buddy_order(page)) > start_pfn)
		return pfn;

	/* Nothing found */
	return start_pfn;
}

static inline void toggle_pageblock_isolate(struct page *page, bool isolate)
{
	if (isolate)
		set_pageblock_isolate(page);
	else
		clear_pageblock_isolate(page);
}

/**
 * __move_freepages_block_isolate - move free pages in block for page isolation
 * @zone: the zone
 * @page: the pageblock page
 * @isolate: to isolate the given pageblock or unisolate it
 *
 * This is similar to move_freepages_block(), but handles the special
 * case encountered in page isolation, where the block of interest
 * might be part of a larger buddy spanning multiple pageblocks.
 *
 * Unlike the regular page allocator path, which moves pages while
 * stealing buddies off the freelist, page isolation is interested in
 * arbitrary pfn ranges that may have overlapping buddies on both ends.
 *
 * This function handles that. Straddling buddies are split into
 * individual pageblocks. Only the block of interest is moved.
 *
 * Returns %true if pages could be moved, %false otherwise.
 */
static bool __move_freepages_block_isolate(struct zone *zone,
		struct page *page, bool isolate)
{
	unsigned long start_pfn, buddy_pfn;
	int from_mt;
	int to_mt;
	struct page *buddy;

	if (isolate == get_pageblock_isolate(page)) {
		VM_WARN_ONCE(1, "%s a pageblock that is already in that state",
			     isolate ? "Isolate" : "Unisolate");
		return false;
	}

	if (!prep_move_freepages_block(zone, page, &start_pfn, NULL, NULL))
		return false;

	/* No splits needed if buddies can't span multiple blocks */
	if (pageblock_order == MAX_PAGE_ORDER)
		goto move;

	buddy_pfn = find_large_buddy(start_pfn);
	buddy = pfn_to_page(buddy_pfn);
	/* We're a part of a larger buddy */
	if (PageBuddy(buddy) && buddy_order(buddy) > pageblock_order) {
		int order = buddy_order(buddy);

		del_page_from_free_list(buddy, zone, order,
					get_pfnblock_migratetype(buddy, buddy_pfn));
		toggle_pageblock_isolate(page, isolate);
		split_large_buddy(zone, buddy, buddy_pfn, order, FPI_NONE);
		return true;
	}

move:
	/* Use MIGRATETYPE_MASK to get non-isolate migratetype */
	if (isolate) {
		from_mt = __get_pfnblock_flags_mask(page, page_to_pfn(page),
						    MIGRATETYPE_MASK);
		to_mt = MIGRATE_ISOLATE;
	} else {
		from_mt = MIGRATE_ISOLATE;
		to_mt = __get_pfnblock_flags_mask(page, page_to_pfn(page),
						  MIGRATETYPE_MASK);
	}

	__move_freepages_block(zone, start_pfn, from_mt, to_mt);
	toggle_pageblock_isolate(pfn_to_page(start_pfn), isolate);

	return true;
}

bool pageblock_isolate_and_move_free_pages(struct zone *zone, struct page *page)
{
	return __move_freepages_block_isolate(zone, page, true);
}

bool pageblock_unisolate_and_move_free_pages(struct zone *zone, struct page *page)
{
	return __move_freepages_block_isolate(zone, page, false);
}

#endif /* CONFIG_MEMORY_ISOLATION */

/*
 * When we are falling back to another migratetype during allocation, should we
 * try to claim an entire block to satisfy further allocations, instead of
 * polluting multiple pageblocks?
 */
static bool should_try_claim_block(unsigned int order, int start_mt)
{
	/*
	 * Leaving this order check is intended, although there is
	 * relaxed order check in next check. The reason is that
	 * we can actually claim the whole pageblock if this condition met,
	 * but, below check doesn't guarantee it and that is just heuristic
	 * so could be changed anytime.
	 */
	if (order >= pageblock_order)
		return true;

	/*
	 * Above a certain threshold, always try to claim, as it's likely there
	 * will be more free pages in the pageblock.
	 */
	if (order >= pageblock_order / 2)
		return true;

	/*
	 * Unmovable/reclaimable allocations would cause permanent
	 * fragmentations if they fell back to allocating from a movable block
	 * (polluting it), so we try to claim the whole block regardless of the
	 * allocation size. Later movable allocations can always steal from this
	 * block, which is less problematic.
	 */
	if (start_mt == MIGRATE_RECLAIMABLE || start_mt == MIGRATE_UNMOVABLE)
		return true;

	if (page_group_by_mobility_disabled)
		return true;

	/*
	 * Movable pages won't cause permanent fragmentation, so when you alloc
	 * small pages, we just need to temporarily steal unmovable or
	 * reclaimable pages that are closest to the request size. After a
	 * while, memory compaction may occur to form large contiguous pages,
	 * and the next movable allocation may not need to steal.
	 */
	return false;
}

/*
 * Check whether there is a suitable fallback freepage with requested order.
 * If claimable is true, this function returns fallback_mt only if
 * we would do this whole-block claiming. This would help to reduce
 * fragmentation due to mixed migratetype pages in one pageblock.
 */
int find_suitable_fallback(struct free_area *area, unsigned int order,
			   int migratetype, bool claimable)
{
	int i;

	if (claimable && !should_try_claim_block(order, migratetype))
		return -2;

	if (area->nr_free == 0)
		return -1;

	for (i = 0; i < MIGRATE_PCPTYPES - 1 ; i++) {
		int fallback_mt = fallbacks[migratetype][i];

		if (!free_area_empty(area, fallback_mt))
			return fallback_mt;
	}

	return -1;
}

/*
 * claim_whole_block - claim a free block (>= pageblock_order) for a new type
 * @zone: zone containing the page
 * @page: free page to claim
 * @current_order: order of the free page
 * @order: requested allocation order
 * @new_type: migratetype to assign
 * @old_type: current migratetype of the block (for free list removal)
 *
 * Handle the PB_all_free → used transition, change the pageblock
 * migratetype, split the block down to @order, and return the page.
 * Used by both the claim fallback path and __rmqueue_smallest when
 * reclaiming free pageblocks from tainted superpageblocks.
 */
static struct page *
claim_whole_block(struct zone *zone, struct page *page,
		  int current_order, int order, int new_type, int old_type)
{
	struct superpageblock *sb;
	unsigned int nr_added;
	unsigned long pb_pfn;

	VM_WARN_ON_ONCE(current_order < order);

	for (pb_pfn = page_to_pfn(page);
	     pb_pfn < page_to_pfn(page) + (1 << current_order);
	     pb_pfn += pageblock_nr_pages) {
		struct page *pb_page = pfn_to_page(pb_pfn);

		if (get_pfnblock_bit(pb_page, pb_pfn, PB_all_free)) {
			clear_pfnblock_bit(pb_page, pb_pfn, PB_all_free);
			superpageblock_pb_now_used(pb_page);
		}
		__spb_set_has_type(pb_page, new_type);
	}

	del_page_from_free_list(page, zone, current_order, old_type);
	change_pageblock_range(page, current_order, new_type);
	nr_added = expand(zone, page, order, current_order, new_type);
	account_freepages(zone, nr_added, new_type);

	/* Single list update after all pageblocks processed */
	sb = pfn_to_superpageblock(zone, page_to_pfn(page));
	if (sb)
		spb_update_list(sb);
	return page;
}

/*
 * This function implements actual block claiming behaviour. If order is large
 * enough, we can claim the whole pageblock for the requested migratetype. If
 * not, we check the pageblock for constituent pages; if at least half of the
 * pages are free or compatible, we can still claim the whole block, so pages
 * freed in the future will be put on the correct free list.
 *
 * @from_tainted_spb: caller has already verified the block lives in a tainted
 * superpageblock, where SPB-level fragmentation has already been accepted.
 * Skip the per-pageblock compatibility threshold so we can absorb non-movable
 * demand into the existing tainted SPB instead of tainting a fresh clean one.
 */
static struct page *
try_to_claim_block(struct zone *zone, struct page *page,
		   int current_order, int order, int start_type,
		   int block_type, unsigned int alloc_flags,
		   bool from_tainted_spb)
{
	int free_pages, movable_pages, alike_pages;
#ifdef CONFIG_COMPACTION
	struct superpageblock *sb;
#endif
	unsigned long start_pfn;

	/*
	 * Don't steal from pageblocks that are isolated for
	 * evacuation — that would undo the work in progress.
	 */
	if (get_pageblock_isolate(page)) {
		trace_spb_claim_block_refused(page, start_type, block_type,
					      SPB_CLAIM_REFUSED_ISOLATE);
		return NULL;
	}

	/*
	 * Never steal from CMA pageblocks.  CMA pages freed through
	 * PCP may land on the MOVABLE free list (PCP caches the
	 * allocation-time migratetype), making them visible to the
	 * fallback search.  Stealing would corrupt CMA by changing
	 * the pageblock type away from MIGRATE_CMA.
	 */
	if (is_migrate_cma(get_pageblock_migratetype(page))) {
		trace_spb_claim_block_refused(page, start_type, block_type,
					      SPB_CLAIM_REFUSED_CMA);
		return NULL;
	}

	/* Take ownership for orders >= pageblock_order */
	if (current_order >= pageblock_order)
		return claim_whole_block(zone, page, current_order, order,
					start_type, block_type);

	/* moving whole block can fail due to zone boundary conditions */
	if (!prep_move_freepages_block(zone, page, &start_pfn, &free_pages,
				       &movable_pages)) {
		trace_spb_claim_block_refused(page, start_type, block_type,
					      SPB_CLAIM_REFUSED_ZONE_BOUNDARY);
		return NULL;
	}

	/*
	 * Determine how many pages are compatible with our allocation.
	 * For movable allocation, it's the number of movable pages which
	 * we just obtained. For other types it's a bit more tricky.
	 */
	if (start_type == MIGRATE_MOVABLE) {
		alike_pages = movable_pages;
	} else {
		/*
		 * If we are falling back a RECLAIMABLE or UNMOVABLE allocation
		 * to MOVABLE pageblock, consider all non-movable pages as
		 * compatible. If it's UNMOVABLE falling back to RECLAIMABLE or
		 * vice versa, be conservative since we can't distinguish the
		 * exact migratetype of non-movable pages.
		 */
		if (block_type == MIGRATE_MOVABLE)
			alike_pages = pageblock_nr_pages
						- (free_pages + movable_pages);
		else
			alike_pages = 0;
	}
	/*
	 * If a sufficient number of pages in the block are either free or of
	 * compatible migratability as our allocation, claim the whole block.
	 * The compatibility threshold protects clean MOVABLE pageblocks from
	 * being relabeled when most of their pages are still in-use movable
	 * allocations. Inside a tainted SPB the protection is unnecessary:
	 * fragmentation has already been accepted at the SPB level, and
	 * relabeling is much cheaper than tainting a fresh clean SPB.
	 *
	 * UNMOVABLE<->RECLAIMABLE cross-type claims override these rules:
	 * once mixed, sticky pinholes of one type prevent the other from
	 * coalescing into useful higher-order free chunks even after drain.
	 * Only relabel a fully-free PB in that case, regardless of whether
	 * the SPB is tainted.
	 */
	if (noncompatible_cross_type(start_type, block_type)) {
		if (free_pages != pageblock_nr_pages) {
			trace_spb_claim_block_refused(page, start_type,
				block_type,
				SPB_CLAIM_REFUSED_CROSS_TYPE_NOT_FREE);
			return NULL;
		}
	} else if (!from_tainted_spb &&
		   free_pages + alike_pages < (1 << (pageblock_order-1)) &&
		   !page_group_by_mobility_disabled) {
		trace_spb_claim_block_refused(page, start_type, block_type,
			SPB_CLAIM_REFUSED_INSUFFICIENT_COMPAT);
		return NULL;
	}

	__move_freepages_block(zone, start_pfn, block_type, start_type);
	set_pageblock_migratetype(pfn_to_page(start_pfn), start_type);
#ifdef CONFIG_COMPACTION
	/*
	 * Track actual page contents in pageblock flags and update
	 * superpageblock counters so the SPB moves to the correct
	 * fullness list for steering.
	 *
	 * For cross-type UNMOVABLE<->RECLAIMABLE relabel (which by the
	 * predicate above only fires on a fully-free PB), the inherited
	 * PB_has_<block_type> bit is stale — there are no in-use pages
	 * of that type. Clear it so the resulting PB is unmixed.
	 */
	__spb_set_has_type(pfn_to_page(start_pfn), start_type);
	if (block_type != start_type) {
		if (noncompatible_cross_type(start_type, block_type))
			__spb_clear_has_type(pfn_to_page(start_pfn), block_type);
		else
			__spb_set_has_type(pfn_to_page(start_pfn), block_type);
	}

	sb = pfn_to_superpageblock(zone, start_pfn);
	if (sb)
		spb_update_list(sb);
#endif
	return __rmqueue_smallest(zone, order, start_type, 0, NULL);
}

/*
 * Search per-superpageblock free lists for a page of a fallback migratetype.
 * Sub-pageblock-order free pages live on superpageblock free lists, not zone
 * free lists, so __rmqueue_claim and __rmqueue_steal need this helper to
 * find fallback pages at those orders.
 *
 * For unmovable/reclaimable allocations, prefer tainted superpageblocks to
 * keep clean ones clean for future large contiguous allocations.
 * For movable allocations, prefer clean superpageblocks to keep movable
 * pages consolidated and superpageblocks homogeneous.
 *
 * @search_cats: bitmask controlling which categories to search.
 *   bit 0: search the preferred category (tainted for unmov, clean for mov)
 *   bit 1: search empty superpageblocks
 *   bit 2: search the fallback category (clean for unmov, tainted for mov)
 * All bits set (0x7) gives the original behavior.
 */
#define SB_SEARCH_PREFERRED	(1 << 0)
#define SB_SEARCH_EMPTY		(1 << 1)
#define SB_SEARCH_FALLBACK	(1 << 2)
#define SB_SEARCH_ALL		(SB_SEARCH_PREFERRED | SB_SEARCH_EMPTY | SB_SEARCH_FALLBACK)
/*
 * Skip UNMOVABLE<->RECLAIMABLE cross-type fallback. Used by the steal
 * path to prevent landing single foreign-type pages into a PB labeled
 * with the other non-movable type — a steal does not relabel the PB
 * so cross-type stealing creates permanent mixing.
 */
#define SB_SKIP_CROSS_TYPE	(1 << 3)

static struct page *
__rmqueue_sb_find_fallback(struct zone *zone, unsigned int order,
			   int start_migratetype, int *fallback_mt,
			   unsigned int search_cats)
{
	int full, i;
	struct superpageblock *sb;
	/*
	 * Category search order: 2 passes.
	 * Movable: clean, tainted.  Others: tainted, clean.
	 */
	static const enum sb_category cat_order[2][2] = {
		[0] = { SB_TAINTED, SB_CLEAN },  /* unmovable/reclaimable */
		[1] = { SB_CLEAN, SB_TAINTED },   /* movable */
	};
	int movable = (start_migratetype == MIGRATE_MOVABLE) ? 1 : 0;

	/* Pass 0: preferred category */
	if (search_cats & SB_SEARCH_PREFERRED) {
		enum sb_category cat = cat_order[movable][0];

		for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
			list_for_each_entry(sb,
					    &zone->spb_lists[cat][full], list) {
				struct free_area *area =
					&sb->free_area[order];

				if (movable && cat == SB_TAINTED &&
				    sb->nr_free <= spb_tainted_reserve(sb))
					continue;

				for (i = 0; i < MIGRATE_PCPTYPES - 1; i++) {
					int fmt = fallbacks[start_migratetype][i];
					struct page *page;

					if ((search_cats & SB_SKIP_CROSS_TYPE) &&
					    noncompatible_cross_type(start_migratetype, fmt))
						continue;

					page = get_page_from_free_area(area,
								       fmt);
					if (page) {
						*fallback_mt = fmt;
						return page;
					}
				}
			}
		}
	}

	/* Empty superpageblocks: between preferred and fallback */
	if (search_cats & SB_SEARCH_EMPTY) {
		list_for_each_entry(sb, &zone->spb_empty, list) {
			struct free_area *area =
				&sb->free_area[order];

			for (i = 0; i < MIGRATE_PCPTYPES - 1; i++) {
				int fmt = fallbacks[start_migratetype][i];
				struct page *page;

				if ((search_cats & SB_SKIP_CROSS_TYPE) &&
				    noncompatible_cross_type(start_migratetype, fmt))
					continue;

				page = get_page_from_free_area(area,
							       fmt);
				if (page) {
					*fallback_mt = fmt;
					return page;
				}
			}
		}
	}

	/* Pass 1: fallback category */
	if (search_cats & SB_SEARCH_FALLBACK) {
		enum sb_category cat = cat_order[movable][1];

		for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
			list_for_each_entry(sb,
					    &zone->spb_lists[cat][full], list) {
				struct free_area *area =
					&sb->free_area[order];

				if (movable && cat == SB_TAINTED &&
				    sb->nr_free <= spb_tainted_reserve(sb))
					continue;

				for (i = 0; i < MIGRATE_PCPTYPES - 1; i++) {
					int fmt = fallbacks[start_migratetype][i];
					struct page *page;

					if ((search_cats & SB_SKIP_CROSS_TYPE) &&
					    noncompatible_cross_type(start_migratetype, fmt))
						continue;

					page = get_page_from_free_area(area,
								       fmt);
					if (page) {
						*fallback_mt = fmt;
						return page;
					}
				}
			}
		}
	}

	return NULL;
}

/*
 * Try to allocate from some fallback migratetype by claiming the entire block,
 * i.e. converting it to the allocation's start migratetype.
 *
 * Search by category first, then by order within each category, to avoid
 * claiming clean/empty superpageblocks when tainted ones still have space
 * at smaller orders.
 *
 * The use of signed ints for order and current_order is a deliberate
 * deviation from the rest of this file, to make the for loop
 * condition simpler.
 */
static __always_inline struct page *
__rmqueue_claim(struct zone *zone, int order, int start_migratetype,
						unsigned int alloc_flags)
{
	int current_order;
	int min_order = order;
	int nofrag_min_order = order;
	struct page *page;
	int fallback_mt;
	static const unsigned int cat_search[] = {
		SB_SEARCH_PREFERRED,
		SB_SEARCH_EMPTY,
		SB_SEARCH_FALLBACK,
	};
	int c;

	/*
	 * Do not steal pages from freelists belonging to other pageblocks
	 * i.e. orders < pageblock_order. If there are no local zones free,
	 * the zonelists will be reiterated without ALLOC_NOFRAGMENT.
	 *
	 * Only apply this restriction to empty and clean superpageblocks.
	 * Claiming within already-tainted superpageblocks does not cause
	 * new fragmentation, and skipping them wastes free space that
	 * could prevent tainting clean superpageblocks.
	 *
	 * When ALLOC_NOFRAGMENT is set, skip empty and clean superpageblocks
	 * entirely to avoid tainting them. The slowpath will try reclaim and
	 * compaction first, and only drop ALLOC_NOFRAGMENT as a last resort.
	 */
	if (order < pageblock_order && alloc_flags & ALLOC_NOFRAGMENT)
		nofrag_min_order = pageblock_order;

	/*
	 * Find the largest available free page in a fallback migratetype.
	 * Search each superpageblock category across all orders before
	 * moving to the next category, so that smaller blocks in tainted
	 * superpageblocks are preferred over larger blocks in empty/clean
	 * ones.
	 */
	for (c = 0; c < ARRAY_SIZE(cat_search); c++) {
		/*
		 * When avoiding fragmentation, do not search clean/empty
		 * superpageblocks for fallback pages. Tainting a clean SPB
		 * is the worst outcome — better to fail and let the slowpath
		 * try reclaim and compaction in already-tainted SPBs first.
		 */
		if ((alloc_flags & ALLOC_NOFRAGMENT) &&
		    cat_search[c] != SB_SEARCH_PREFERRED)
			continue;

		/*
		 * For the preferred category (tainted SPBs for non-movable),
		 * search all orders down to the allocation order on systems
		 * with enough superpageblocks that we can afford to write off
		 * tainted ones. These SPBs are already tainted, so sub-pageblock
		 * stealing doesn't cause additional fragmentation.
		 *
		 * On small systems, keep the pageblock_order floor to preserve
		 * MOVABLE capacity within tainted SPBs — see comment at
		 * SPB_AGGRESSIVE_THRESHOLD.
		 */
		min_order = (cat_search[c] == SB_SEARCH_PREFERRED &&
			     zone->nr_superpageblocks > SPB_AGGRESSIVE_THRESHOLD) ?
			    order : nofrag_min_order;

		for (current_order = MAX_PAGE_ORDER;
		     current_order >= min_order; --current_order) {
			if (!should_try_claim_block(current_order,
						    start_migratetype))
				break;
			page = __rmqueue_sb_find_fallback(zone, current_order,
						start_migratetype,
						&fallback_mt, cat_search[c]);
			if (!page)
				continue;

			/*
			 * About to claim from an empty or clean superpageblock
			 * for a non-movable allocation -- this taints a fresh
			 * SPB.  Defer an evacuation pass over the tainted pool
			 * so subsequent allocations can reclaim freed
			 * pageblocks instead of repeating this fallback. Also
			 * kick a slab shrink so the tainted pool gets fresh
			 * headroom (movable evac alone can't free pages held
			 * by slab).
			 */
			if (cat_search[c] != SB_SEARCH_PREFERRED &&
			    start_migratetype != MIGRATE_MOVABLE) {
				queue_spb_evacuate(zone, order,
						   start_migratetype);
				queue_spb_slab_shrink(zone);
			}

			page = try_to_claim_block(zone, page, current_order,
						  order, start_migratetype,
						  fallback_mt, alloc_flags,
						  false);
			if (page) {
				trace_mm_page_alloc_extfrag(page, order,
					current_order, start_migratetype,
					fallback_mt);
				return page;
			}
		}
	}

	return NULL;
}

/*
 * Try to steal a single page from some fallback migratetype. Leave the rest of
 * the block as its current migratetype, potentially causing fragmentation.
 */
static __always_inline struct page *
__rmqueue_steal(struct zone *zone, int order, int start_migratetype,
		unsigned int alloc_flags)
{
	struct superpageblock *sb;
	int current_order;
	struct page *page;
	int fallback_mt;
	unsigned int search_cats;

	/*
	 * When ALLOC_NOFRAG_TAINTED_OK is set, only steal from tainted
	 * SPBs to avoid tainting clean ones. Otherwise search all categories.
	 *
	 * Always skip UNMOVABLE<->RECLAIMABLE cross-type fallback. The steal
	 * path takes a single page without relabeling its PB, so a cross-type
	 * steal would land an UNMOVABLE page in a RECLAIMABLE-labeled PB
	 * (or vice versa) and create permanent mixing. Falling through to
	 * MIGRATE_MOVABLE (the second fallback) is preferable.
	 */
	if (alloc_flags & ALLOC_NOFRAG_TAINTED_OK)
		search_cats = SB_SEARCH_PREFERRED;
	else
		search_cats = SB_SEARCH_PREFERRED | SB_SEARCH_FALLBACK;
	search_cats |= SB_SKIP_CROSS_TYPE;

	/*
	 * Search per-superpageblock free lists for fallback migratetypes.
	 * Superpageblocks are always enabled for populated zones.
	 */
	for (current_order = order; current_order < NR_PAGE_ORDERS; current_order++) {
		page = __rmqueue_sb_find_fallback(zone, current_order,
					start_migratetype,
					&fallback_mt,
					search_cats);

		if (!page)
			continue;

		page_del_and_expand(zone, page, order, current_order, fallback_mt);

		/*
		 * page_del_and_expand recorded PB_has_<fallback_mt> for the
		 * source free list type. Also record the actual allocation
		 * type so evacuation and defrag can find these pages.
		 *
		 * For example, a MOVABLE allocation stealing from an
		 * UNMOVABLE free list must set PB_has_movable so the
		 * pageblock is visible to evacuate_pageblock() and
		 * spb_defrag_tainted(). __spb_set_has_type is idempotent:
		 * it only increments the SPB counter on the 0->1 bit
		 * transition.
		 */
		if (fallback_mt != start_migratetype) {
			__spb_set_has_type(page, start_migratetype);
			sb = pfn_to_superpageblock(zone,
						   page_to_pfn(page));
			if (sb)
				spb_update_list(sb);
		}

		trace_mm_page_alloc_extfrag(page, order, current_order,
					    start_migratetype, fallback_mt);
		return page;
	}

	return NULL;
}

enum rmqueue_mode {
	RMQUEUE_NORMAL,
	RMQUEUE_CMA,
	RMQUEUE_CLAIM,
	RMQUEUE_STEAL,
};

/*
 * Do the hard work of removing an element from the buddy allocator.
 * Call me with the zone->lock already held.
 */
static __always_inline struct page *
__rmqueue(struct zone *zone, unsigned int order, int migratetype,
	  unsigned int alloc_flags, enum rmqueue_mode *mode)
{
	struct spb_tainted_walk walk = { };
	struct spb_tainted_walk *walkp = NULL;
	struct page *page;

	/*
	 * Track tainted-SPB state for non-movable, non-CMA callers that
	 * signaled they have a cheap fallback (atomic shape or explicit
	 * NORETRY). We use that to refuse a fragmenting CLAIM/STEAL when a
	 * tainted SPB still has free pageblocks waiting to be evacuated.
	 *
	 * Force *mode back to RMQUEUE_NORMAL so the walk + refusal check
	 * runs on every call. rmqueue_bulk Phase 3 chains many __rmqueue
	 * calls reusing *mode; without this reset, a single successful
	 * RMQUEUE_CLAIM/STEAL on the first iteration would let every
	 * subsequent iteration skip the case RMQUEUE_NORMAL block and taint
	 * additional clean SPBs unchecked.
	 */
	if (migratetype != MIGRATE_MOVABLE && !is_migrate_cma(migratetype) &&
	    (alloc_flags & ALLOC_HIGHORDER_OPTIONAL)) {
		walkp = &walk;
		*mode = RMQUEUE_NORMAL;
	}

	if (IS_ENABLED(CONFIG_CMA)) {
		/*
		 * Balance movable allocations between regular and CMA areas by
		 * allocating from CMA when over half of the zone's free memory
		 * is in the CMA area.
		 */
		if (alloc_flags & ALLOC_CMA &&
		    zone_page_state(zone, NR_FREE_CMA_PAGES) >
		    zone_page_state(zone, NR_FREE_PAGES) / 2) {
			page = __rmqueue_cma_fallback(zone, order);
			if (page)
				return page;
		}
	}

	/*
	 * First try the freelists of the requested migratetype, then try
	 * fallbacks modes with increasing levels of fragmentation risk.
	 *
	 * The fallback logic is expensive and rmqueue_bulk() calls in
	 * a loop with the zone->lock held, meaning the freelists are
	 * not subject to any outside changes. Remember in *mode where
	 * we found pay dirt, to save us the search on the next call.
	 */
	switch (*mode) {
	case RMQUEUE_NORMAL:
		page = __rmqueue_smallest(zone, order, migratetype,
					  alloc_flags, walkp);
		if (page)
			return page;
		/*
		 * Refuse to fragment a clean SPB when a tainted SPB already
		 * holds free pages or a free pageblock that could absorb
		 * this allocation after evacuation. The caller has a cheap
		 * fallback (lower-order retry, vmalloc, single-page fragment,
		 * drop the packet, etc.) — better that than tainting fresh
		 * capacity. Pre-Pass-3 evac trigger in __rmqueue_smallest
		 * already kicked deferred eviction.
		 */
		if (walkp && (walk.saw_free_pages || walk.saw_free_pb)) {
			count_vm_event(SPB_HIGHORDER_REFUSED);
			return NULL;
		}
		fallthrough;
	case RMQUEUE_CMA:
		if (alloc_flags & ALLOC_CMA) {
			page = __rmqueue_cma_fallback(zone, order);
			if (page) {
				*mode = RMQUEUE_CMA;
				return page;
			}
		}
		fallthrough;
	case RMQUEUE_CLAIM:
		page = __rmqueue_claim(zone, order, migratetype, alloc_flags);
		if (page) {
			/* Replenished preferred freelist, back to normal mode. */
			*mode = RMQUEUE_NORMAL;
			return page;
		}
		fallthrough;
	case RMQUEUE_STEAL:
		if (!(alloc_flags & ALLOC_NOFRAGMENT) ||
		    (alloc_flags & ALLOC_NOFRAG_TAINTED_OK)) {
			page = __rmqueue_steal(zone, order, migratetype,
					       alloc_flags);
			if (page) {
				*mode = RMQUEUE_STEAL;
				return page;
			}
		}
	}
	return NULL;
}

/*
 * Obtain a specified number of elements from the buddy allocator, all
 * under a single hold of the lock, for efficiency.  Add them to the
 * freelist of @pcp.
 *
 * When @pcp is non-NULL and @count > 1 (normal pageset), uses a multi-phase
 * approach:
 *   Phase 0:   Recover previously owned, partially drained blocks.
 *   Phase 1:   Acquire whole pageblocks, claim ownership, set PagePCPBuddy.
 *              These pages are eligible for PCP-level buddy merging.
 *   Phase 2:   Adopt partial pageblocks from tainted SPBs (non-movable only).
 *              Claims ownership so Pass 0 can recover buddy entries later.
 *   Phase 3:   Fall back to __rmqueue() with migratetype fallback.
 *              No ownership claim, no PagePCPBuddy, no PCP-level merging.
 *
 * When @pcp is NULL or @count <= 1 (boot pageset), acquires individual
 * pages of the requested order directly.
 *
 * Returns %true if at least some pages were acquired.
 */
static bool rmqueue_bulk(struct zone *zone, unsigned int order,
			 unsigned long count,
			 int migratetype, unsigned int alloc_flags,
			 struct per_cpu_pages *pcp)
{
	unsigned long pages_needed = count << order;
	enum rmqueue_mode rmqm = RMQUEUE_NORMAL;
	struct pageblock_data *pbd, *tmp;
	int cpu = smp_processor_id();
	unsigned long refilled = 0;
	unsigned long flags;
	int o;

	if (unlikely(alloc_flags & ALLOC_TRYLOCK)) {
		if (!spin_trylock_irqsave(&zone->lock, flags))
			return false;
	} else {
		spin_lock_irqsave(&zone->lock, flags);
	}

	if (!pcp || count <= 1)
		goto phase3;

	/*
	 * Phase 0: Recover fragments from owned blocks.
	 *
	 * The owned_blocks list tracks blocks that have fragments
	 * sitting in zone buddy (put there by drains). Pull matching
	 * fragments back to PCP with PagePCPBuddy so they participate
	 * in merging, instead of claiming fresh blocks and spreading
	 * fragmentation further.
	 *
	 * Only recover blocks matching the requested migratetype.
	 * After recovery, remove the block from the list -- the drain
	 * path re-adds it if new fragments arrive.
	 */
	list_for_each_entry_safe(pbd, tmp, &pcp->owned_blocks, cpu_node) {
		unsigned long base_pfn, pfn;
		int block_mt;

		base_pfn = pbd->block_pfn;
		block_mt = pbd_migratetype(pbd);
		if (block_mt != migratetype)
			continue;

		for (pfn = base_pfn; pfn < base_pfn + pageblock_nr_pages;) {
			struct page *page = pfn_to_page(pfn);

			if (!PageBuddy(page)) {
				pfn++;
				continue;
			}

			o = buddy_order(page);
			del_page_from_free_list(page, zone, o, block_mt);
			__SetPagePCPBuddy(page);
			pcp_enqueue_tail(pcp, page, block_mt, o);
			refilled += 1 << o;
			pfn += 1 << o;
		}

		list_del_init(&pbd->cpu_node);

		if (refilled >= pages_needed)
			goto out;
	}

	/*
	 * Phase 1: Try whole pageblocks. Fast path for unfragmented
	 * zones. Claim ownership and set PagePCPBuddy so these pages
	 * are eligible for PCP-level merging.
	 *
	 * Only grab blocks that fit within the refill budget. On
	 * small zones, pages_needed can be less than a whole
	 * pageblock; skip to smaller blocks or individual pages to
	 * avoid overshooting the PCP high watermark.
	 *
	 * For movable allocations, prefer pageblocks from the
	 * fullest clean superpageblock to pack allocations and
	 * preserve empty superpageblocks for 1GB hugepages.
	 *
	 * For non-movable allocations, force ALLOC_NOFRAGMENT so
	 * __rmqueue cannot steal a whole pageblock out of a clean
	 * SPB. Stealing is the worst possible outcome for a bulk
	 * refill: a single network or slab burst can taint dozens
	 * of clean pageblocks. Phase 2 will adopt sub-pageblock
	 * fragments from tainted SPBs before Phase 3 falls back to
	 * the original alloc_flags (which may eventually steal at
	 * the requested order, a much smaller fragmentation event).
	 */
	while (refilled + pageblock_nr_pages <= pages_needed) {
		unsigned int p1_alloc_flags = alloc_flags;
		struct page *page = NULL;

		if (migratetype == MIGRATE_MOVABLE) {
			struct superpageblock *sb;

			sb = sb_preferred_for_movable(zone);
			if (sb)
				page = __rmqueue_from_sb(zone, pageblock_order,
							 migratetype, sb);
		} else if (!is_migrate_cma(migratetype)) {
			p1_alloc_flags = (p1_alloc_flags | ALLOC_NOFRAGMENT) &
					 ~ALLOC_NOFRAG_TAINTED_OK;
		}
		if (!page)
			page = __rmqueue(zone, pageblock_order,
					 migratetype,
					 p1_alloc_flags, &rmqm);
		if (!page)
			break;

		set_pcpblock_owner(page, cpu);
		__SetPagePCPBuddy(page);
		pcp_enqueue_tail(pcp, page, migratetype, pageblock_order);
		refilled += 1 << pageblock_order;
	}
	if (refilled >= pages_needed)
		goto out;

	/*
	 * Phase 2: Adopt partial pageblocks from tainted SPBs.
	 *
	 * Phase 1 only grabs whole free pageblocks. When a tainted SPB
	 * has partially-used pageblocks with free sub-pageblock buddy
	 * entries, Phase 1 can't use them. Phase 3 can find them via
	 * __rmqueue_smallest, but without ownership or PCPBuddy marking,
	 * so they fragment further on drain.
	 *
	 * This phase bridges the gap: find a sub-pageblock free entry
	 * in a tainted SPB and claim ownership of its pageblock. Pass 0
	 * will pick up remaining buddy entries on subsequent refills.
	 *
	 * Only for unmovable/reclaimable — movable should use clean SPBs.
	 */
	if (migratetype != MIGRATE_MOVABLE &&
	    !is_migrate_cma(migratetype)) {
		enum sb_fullness full;

		for (full = SB_FULL; full < __NR_SB_FULLNESS; full++) {
			struct superpageblock *sb;

			list_for_each_entry(sb,
				&zone->spb_lists[SB_TAINTED][full], list) {
				struct page *page;
				int found_order = -1;
				bool claim_pb;

				if (sb->nr_free_pages < pageblock_nr_pages / 4)
					continue;

				/*
				 * Find a sub-pageblock free entry for our
				 * migratetype, starting from the largest order.
				 */
				for (o = pageblock_order - 1; o >= order; o--) {
					struct free_area *area;

					area = &sb->free_area[o];
					page = get_page_from_free_area(
						area, migratetype);
					if (page) {
						found_order = o;
						break;
					}
				}
				if (found_order < 0)
					continue;

				/*
				 * Found a free fragment in a tainted SPB. Take
				 * it from the buddy.
				 *
				 * If the source pageblock is unowned, claim it:
				 * mark our pages PagePCPBuddy and register the
				 * block on owned_blocks so Pass 0 can recover
				 * remaining fragments on future refills.
				 *
				 * If the source pageblock is already owned by
				 * some CPU (us or another), take the page as a
				 * plain non-PCPBuddy fragment — the same way
				 * Phase 3 / __rmqueue_smallest would. Setting
				 * PagePCPBuddy here would let two CPUs hold
				 * PCPBuddy pages from the same pageblock, and
				 * the PCP merge pass could then corrupt the
				 * other CPU's PCP list.
				 *
				 * Set PB_has_<migratetype> either way (bypasses
				 * page_del_and_expand which normally does the
				 * PB_has tracking); idempotent if already set.
				 */
				pbd = pfn_to_pageblock(page,
						       page_to_pfn(page));
				claim_pb = (pbd->cpu == 0);

				del_page_from_free_list(page, zone,
							found_order,
							migratetype);
				__spb_set_has_type(page, migratetype);
				if (claim_pb) {
					set_pcpblock_owner(page, cpu);
					__SetPagePCPBuddy(page);
				}
				pcp_enqueue_tail(pcp, page, migratetype,
						 found_order);
				refilled += 1 << found_order;

				/*
				 * Register for Phase 0 recovery so future
				 * drains from this pageblock can be swept
				 * back efficiently. Only meaningful when we
				 * actually claimed ownership above.
				 */
				if (claim_pb && list_empty(&pbd->cpu_node))
					list_add(&pbd->cpu_node,
						 &pcp->owned_blocks);

				if (refilled >= pages_needed)
					goto out;
			}
		}
	}

	/*
	 * Phase 3: Last resort. Use __rmqueue() which does
	 * migratetype fallback. Cache the pages on PCP to still
	 * amortize future zone lock acquisitions.
	 *
	 * No ownership claim or PagePCPBuddy - these fragments
	 * drain individually to zone buddy.
	 *
	 * Boot pagesets (count <= 1) jump here directly.
	 */
phase3:
	while (refilled < pages_needed) {
		struct page *page = __rmqueue(zone, order, migratetype,
					      alloc_flags, &rmqm);
		if (!page)
			break;

		/*
		 * Split buddy pages returned by expand() are received here in
		 * physical page order. The page is added to the tail of
		 * caller's list. From the callers perspective, the linked list
		 * is ordered by page number under some conditions. This is
		 * useful for IO devices that can forward direction from the
		 * head, thus also in the physical page order. This is useful
		 * for IO devices that can merge IO requests if the physical
		 * pages are ordered properly.
		 */
		pcp_enqueue_tail(pcp, page, migratetype, order);
		refilled += 1 << order;
	}

out:
	spin_unlock_irqrestore(&zone->lock, flags);
	return refilled;
}

/*
 * Called from the vmstat counter updater to decay the PCP high.
 * Return whether there are addition works to do.
 */
bool decay_pcp_high(struct zone *zone, struct per_cpu_pages *pcp)
{
	int high_min, to_drain, to_drain_batched, batch;
	unsigned long UP_flags;
	bool todo = false;

	high_min = READ_ONCE(pcp->high_min);
	batch = READ_ONCE(pcp->batch);
	/*
	 * Decrease pcp->high periodically to try to free possible
	 * idle PCP pages.  And, avoid to free too many pages to
	 * control latency.  This caps pcp->high decrement too.
	 */
	if (pcp->high > high_min) {
		pcp->high = max3(pcp->count - (batch << CONFIG_PCP_BATCH_SCALE_MAX),
				 pcp->high - (pcp->high >> 3), high_min);
		if (pcp->high > high_min)
			todo = true;
	}

	to_drain = pcp->count - pcp->high;
	while (to_drain > 0) {
		to_drain_batched = min(to_drain, batch);
		pcp_spin_lock_maybe_irqsave(pcp, UP_flags);
		free_pcppages_bulk(zone, to_drain_batched, pcp);
		pcp_spin_unlock_maybe_irqrestore(pcp, UP_flags);
		todo = true;

		to_drain -= to_drain_batched;
	}

	return todo;
}

#ifdef CONFIG_NUMA
/*
 * Called from the vmstat counter updater to drain pagesets of this
 * currently executing processor on remote nodes after they have
 * expired.
 */
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp)
{
	unsigned long UP_flags;
	int to_drain, batch;

	batch = READ_ONCE(pcp->batch);
	to_drain = min(pcp->count, batch);
	if (to_drain > 0) {
		pcp_spin_lock_maybe_irqsave(pcp, UP_flags);
		free_pcppages_bulk(zone, to_drain, pcp);
		pcp_spin_unlock_maybe_irqrestore(pcp, UP_flags);
	}
}
#endif

/*
 * Drain pcplists of the indicated processor and zone.
 */
static void drain_pages_zone(unsigned int cpu, struct zone *zone)
{
	struct per_cpu_pages *pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
	unsigned long UP_flags;
	int count;

	do {
		pcp_spin_lock_maybe_irqsave(pcp, UP_flags);
		count = pcp->count;
		if (count) {
			int to_drain = min(count,
				pcp->batch << CONFIG_PCP_BATCH_SCALE_MAX);

			free_pcppages_bulk(zone, to_drain, pcp);
			count -= to_drain;
		}
		pcp_spin_unlock_maybe_irqrestore(pcp, UP_flags);
	} while (count);
}

/*
 * Drain pcplists of all zones on the indicated processor.
 */
static void drain_pages(unsigned int cpu)
{
	struct zone *zone;

	for_each_populated_zone(zone) {
		drain_pages_zone(cpu, zone);
	}
}

/*
 * Spill all of this CPU's per-cpu pages back into the buddy allocator.
 */
void drain_local_pages(struct zone *zone)
{
	int cpu = smp_processor_id();

	if (zone)
		drain_pages_zone(cpu, zone);
	else
		drain_pages(cpu);
}

/*
 * The implementation of drain_all_pages(), exposing an extra parameter to
 * drain on all cpus.
 *
 * drain_all_pages() is optimized to only execute on cpus where pcplists are
 * not empty. The check for non-emptiness can however race with a free to
 * pcplist that has not yet increased the pcp->count from 0 to 1. Callers
 * that need the guarantee that every CPU has drained can disable the
 * optimizing racy check.
 */
static void __drain_all_pages(struct zone *zone, bool force_all_cpus)
{
	int cpu;

	/*
	 * Allocate in the BSS so we won't require allocation in
	 * direct reclaim path for CONFIG_CPUMASK_OFFSTACK=y
	 */
	static cpumask_t cpus_with_pcps;

	/*
	 * Do not drain if one is already in progress unless it's specific to
	 * a zone. Such callers are primarily CMA and memory hotplug and need
	 * the drain to be complete when the call returns.
	 */
	if (unlikely(!mutex_trylock(&pcpu_drain_mutex))) {
		if (!zone)
			return;
		mutex_lock(&pcpu_drain_mutex);
	}

	/*
	 * We don't care about racing with CPU hotplug event
	 * as offline notification will cause the notified
	 * cpu to drain that CPU pcps and on_each_cpu_mask
	 * disables preemption as part of its processing
	 */
	for_each_online_cpu(cpu) {
		struct per_cpu_pages *pcp;
		struct zone *z;
		bool has_pcps = false;

		if (force_all_cpus) {
			/*
			 * The pcp.count check is racy, some callers need a
			 * guarantee that no cpu is missed.
			 */
			has_pcps = true;
		} else if (zone) {
			pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
			if (pcp->count)
				has_pcps = true;
		} else {
			for_each_populated_zone(z) {
				pcp = per_cpu_ptr(z->per_cpu_pageset, cpu);
				if (pcp->count) {
					has_pcps = true;
					break;
				}
			}
		}

		if (has_pcps)
			cpumask_set_cpu(cpu, &cpus_with_pcps);
		else
			cpumask_clear_cpu(cpu, &cpus_with_pcps);
	}

	for_each_cpu(cpu, &cpus_with_pcps) {
		if (zone)
			drain_pages_zone(cpu, zone);
		else
			drain_pages(cpu);
	}

	mutex_unlock(&pcpu_drain_mutex);
}

/*
 * Spill all the per-cpu pages from all CPUs back into the buddy allocator.
 *
 * When zone parameter is non-NULL, spill just the single zone's pages.
 */
void drain_all_pages(struct zone *zone)
{
	__drain_all_pages(zone, false);
}

static int nr_pcp_free(struct per_cpu_pages *pcp, int batch, int high, bool free_high)
{
	int min_nr_free, max_nr_free;

	/* Free as much as possible if batch freeing high-order pages. */
	if (unlikely(free_high))
		return min(pcp->count, batch << CONFIG_PCP_BATCH_SCALE_MAX);

	/* Check for PCP disabled or boot pageset */
	if (unlikely(high < batch))
		return 1;

	/* Leave at least pcp->batch pages on the list */
	min_nr_free = batch;
	max_nr_free = high - batch;

	/*
	 * Increase the batch number to the number of the consecutive
	 * freed pages to reduce zone lock contention.
	 */
	batch = clamp_t(int, pcp->free_count, min_nr_free, max_nr_free);

	return batch;
}

static int nr_pcp_high(struct per_cpu_pages *pcp, struct zone *zone,
		       int batch, bool free_high)
{
	int high, high_min, high_max;

	high_min = READ_ONCE(pcp->high_min);
	high_max = READ_ONCE(pcp->high_max);
	high = pcp->high = clamp(pcp->high, high_min, high_max);

	if (unlikely(!high))
		return 0;

	if (unlikely(free_high)) {
		pcp->high = max(high - (batch << CONFIG_PCP_BATCH_SCALE_MAX),
				high_min);
		return 0;
	}

	/*
	 * If reclaim is active, limit the number of pages that can be
	 * stored on pcp lists
	 */
	if (test_bit(ZONE_RECLAIM_ACTIVE, &zone->flags)) {
		int free_count = max_t(int, pcp->free_count, batch);

		pcp->high = max(high - free_count, high_min);
		return min(batch << 2, pcp->high);
	}

	if (high_min == high_max)
		return high;

	if (test_bit(ZONE_BELOW_HIGH, &zone->flags)) {
		int free_count = max_t(int, pcp->free_count, batch);

		pcp->high = max(high - free_count, high_min);
		high = max(pcp->count, high_min);
	} else if (pcp->count >= high) {
		int need_high = pcp->free_count + batch;

		/* pcp->high should be large enough to hold batch freed pages */
		if (pcp->high < need_high)
			pcp->high = clamp(need_high, high_min, high_max);
	}

	return high;
}

/*
 * Free a page to the PCP and flush excess pages if necessary.
 * Works for both local and remote PCP - caller handles locking.
 * @owned: page is from a PCP-owned block (eligible for merging).
 */
static void free_frozen_page_commit(struct zone *zone,
		struct per_cpu_pages *pcp, struct page *page, int migratetype,
		unsigned int order, fpi_t fpi_flags, bool owned)
{
	int high, batch, to_free;
	bool free_high = false;

	/*
	 * On freeing, reduce the number of pages that are batch allocated.
	 * See nr_pcp_alloc() where alloc_factor is increased for subsequent
	 * allocations.
	 */
	pcp->alloc_factor >>= 1;
	__count_vm_events(PGFREE, 1 << order);
	/*
	 * Only set PagePCPBuddy for pages from owned blocks -- those
	 * are on their home PCP and eligible for buddy merging.
	 * Zone-owned pages are cached on the local PCP for batching
	 * only; the merge pass skips them harmlessly.
	 */
	if (owned)
		__SetPagePCPBuddy(page);
	pcp_enqueue(pcp, page, migratetype, order);

	batch = READ_ONCE(pcp->batch);
	/*
	 * As high-order pages other than THP's stored on PCP can contribute
	 * to fragmentation, limit the number stored when PCP is heavily
	 * freeing without allocation. The remainder after bulk freeing
	 * stops will be drained from vmstat refresh context.
	 */
	if (order && order <= PAGE_ALLOC_COSTLY_ORDER) {
		free_high = (pcp->free_count >= (batch + pcp->high_min / 2) &&
			     (pcp->flags & PCPF_PREV_FREE_HIGH_ORDER) &&
			     (!(pcp->flags & PCPF_FREE_HIGH_BATCH) ||
			      pcp->count >= batch));
		pcp->flags |= PCPF_PREV_FREE_HIGH_ORDER;
	} else if (pcp->flags & PCPF_PREV_FREE_HIGH_ORDER) {
		pcp->flags &= ~PCPF_PREV_FREE_HIGH_ORDER;
	}
	if (pcp->free_count < (batch << CONFIG_PCP_BATCH_SCALE_MAX))
		pcp->free_count += (1 << order);

	if (unlikely(fpi_flags & FPI_TRYLOCK)) {
		/*
		 * Do not attempt to take a zone lock. Let pcp->count get
		 * over high mark temporarily.
		 */
		return;
	}

	high = nr_pcp_high(pcp, zone, batch, free_high);
	if (pcp->count < high)
		return;

	to_free = nr_pcp_free(pcp, batch, high, free_high);
	free_pcppages_bulk(zone, to_free, pcp);

	if (test_bit(ZONE_BELOW_HIGH, &zone->flags) &&
	    zone_watermark_ok(zone, 0, high_wmark_pages(zone),
			      ZONE_MOVABLE, 0)) {
		struct pglist_data *pgdat = zone->zone_pgdat;
		clear_bit(ZONE_BELOW_HIGH, &zone->flags);

		/*
		 * Assume that memory pressure on this node is gone and may be
		 * in a reclaimable state. If a memory fallback node exists,
		 * direct reclaim may not have been triggered, causing a
		 * 'hopeless node' to stay in that state for a while.  Let
		 * kswapd work again by resetting kswapd_failures.
		 */
		if (kswapd_test_hopeless(pgdat) &&
		    next_memory_node(pgdat->node_id) < MAX_NUMNODES)
			kswapd_clear_hopeless(pgdat, KSWAPD_CLEAR_HOPELESS_PCP);
	}
}

/*
 * Free a pcp page
 */
static void __free_frozen_pages(struct page *page, unsigned int order,
				fpi_t fpi_flags)
{
	unsigned long UP_flags;
	struct per_cpu_pages *pcp;
	struct pageblock_data *pbd;
	struct zone *zone;
	unsigned long pfn = page_to_pfn(page);
	int migratetype;
	int owner_cpu, cache_cpu;

	if (!pcp_allowed_order(order)) {
		__free_pages_ok(page, order, fpi_flags);
		return;
	}

	if (!__free_pages_prepare(page, order, fpi_flags))
		return;

	/*
	 * We only track unmovable, reclaimable and movable on pcp lists.
	 * Place ISOLATE pages on the isolated list because they are being
	 * offlined but treat HIGHATOMIC and CMA as movable pages so we can
	 * get those areas back if necessary. Otherwise, we may have to free
	 * excessively into the page allocator
	 */
	zone = page_zone(page);
	pbd = pfn_to_pageblock(page, pfn);
	migratetype = pbd_migratetype(pbd);
	if (unlikely(migratetype >= MIGRATE_PCPTYPES)) {
		if (unlikely(is_migrate_isolate(migratetype))) {
			free_one_page(zone, page, pfn, order, fpi_flags);
			return;
		}
		migratetype = MIGRATE_MOVABLE;
	}

	if (unlikely((fpi_flags & FPI_TRYLOCK) && IS_ENABLED(CONFIG_PREEMPT_RT)
		     && (in_nmi() || in_hardirq()))) {
		add_page_to_zone_llist(zone, page, order);
		return;
	}

	/*
	 * Route page to the owning CPU's PCP for merging, or to
	 * the local PCP for batching (zone-owned pages). Zone-owned
	 * pages are cached without PagePCPBuddy -- the merge pass
	 * skips them, so they're inert on any PCP list and drain
	 * individually to zone buddy.
	 *
	 * Ownership is stable here: it can only change when the
	 * pageblock is complete -- either fully free in zone buddy
	 * (Phase 1 claims) or fully merged on PCP (drain disowns).
	 * Since we hold this page, neither can happen.
	 */
	owner_cpu = pbd->cpu - 1;
	cache_cpu = owner_cpu;
	if (cache_cpu < 0)
		cache_cpu = raw_smp_processor_id();

	pcp = per_cpu_ptr(zone->per_cpu_pageset, cache_cpu);
	/*
	 * Always use trylock: callers may hold locks (e.g. xa_lock via
	 * slab/stack_depot) that are also taken in hardirq context, and
	 * pcp->lock is acquired with IRQs enabled on the allocation side.
	 * A blocking lock here would create an ABBA deadlock potential.
	 */
	if (!spin_trylock_irqsave(&pcp->lock, UP_flags)) {
		free_one_page(zone, page, pfn, order, fpi_flags);
		return;
	}

	if (unlikely(pcp->flags & PCPF_CPU_DEAD)) {
		spin_unlock_irqrestore(&pcp->lock, UP_flags);
		free_one_page(zone, page, pfn, order, fpi_flags);
		return;
	}

	free_frozen_page_commit(zone, pcp, page,
				migratetype, order, fpi_flags,
				cache_cpu == owner_cpu);

	spin_unlock_irqrestore(&pcp->lock, UP_flags);
}

void free_frozen_pages(struct page *page, unsigned int order)
{
#ifdef CONFIG_CONTIG_ALLOC
	/*
	 * If freeing a superpageblock-sized (or larger) range, clear the
	 * contig_allocated flag so the SPB returns to normal allocation.
	 */
	if (order >= SUPERPAGEBLOCK_ORDER) {
		unsigned long pfn = page_to_pfn(page);

		superpageblock_contig_mark(pfn, pfn + (1UL << order), false);
	}
#endif
	__free_frozen_pages(page, order, FPI_NONE);
}

void free_frozen_pages_nolock(struct page *page, unsigned int order)
{
	__free_frozen_pages(page, order, FPI_TRYLOCK);
}

/*
 * Free a batch of folios
 */
void free_unref_folios(struct folio_batch *folios)
{
	unsigned long UP_flags;
	struct per_cpu_pages *pcp = NULL;
	struct zone *locked_zone = NULL;
	int locked_cpu = -1;
	int i, j;

	/* Prepare folios for freeing */
	for (i = 0, j = 0; i < folios->nr; i++) {
		struct folio *folio = folios->folios[i];
		unsigned long pfn = folio_pfn(folio);
		unsigned int order = folio_order(folio);

		if (!__free_pages_prepare(&folio->page, order, FPI_NONE))
			continue;
		/*
		 * Free orders not handled on the PCP directly to the
		 * allocator.
		 */
		if (!pcp_allowed_order(order)) {
			free_one_page(folio_zone(folio), &folio->page,
				      pfn, order, FPI_NONE);
			continue;
		}
		folio->private = (void *)(unsigned long)order;
		if (j != i)
			folios->folios[j] = folio;
		j++;
	}
	folios->nr = j;

	for (i = 0; i < folios->nr; i++) {
		struct folio *folio = folios->folios[i];
		struct zone *zone = folio_zone(folio);
		unsigned long pfn = folio_pfn(folio);
		unsigned int order = (unsigned long)folio->private;
		struct pageblock_data *pbd;
		int migratetype;
		int owner_cpu, cache_cpu;

		folio->private = NULL;
		pbd = pfn_to_pageblock(&folio->page, pfn);
		migratetype = pbd_migratetype(pbd);
		owner_cpu = pbd->cpu - 1;
		cache_cpu = owner_cpu;
		if (cache_cpu < 0)
			cache_cpu = raw_smp_processor_id();

		/*
		 * Re-lock needed if zone changed, page is isolate,
		 * or target CPU changed.
		 */
		if (zone != locked_zone ||
		    is_migrate_isolate(migratetype) ||
		    cache_cpu != locked_cpu) {
			if (pcp) {
				spin_unlock_irqrestore(&pcp->lock, UP_flags);
				locked_zone = NULL;
				locked_cpu = -1;
				pcp = NULL;
			}

			/*
			 * Free isolated pages directly to the
			 * allocator, see comment in free_frozen_pages.
			 */
			if (is_migrate_isolate(migratetype)) {
				free_one_page(zone, &folio->page, pfn,
					      order, FPI_NONE);
				continue;
			}

			pcp = per_cpu_ptr(zone->per_cpu_pageset,
					  cache_cpu);
			/*
			 * Use trylock when not in task context (IRQ,
			 * softirq) to avoid spinning with IRQs
			 * disabled. In task context, spin -- brief
			 * contention on a per-CPU lock beats the
			 * unbatched zone->lock fallback.
			 */
			if (!in_task()) {
				if (unlikely(!spin_trylock_irqsave(
						&pcp->lock, UP_flags))) {
					pcp = NULL;
					free_one_page(zone, &folio->page, pfn,
						      order, FPI_NONE);
					continue;
				}
			} else {
				spin_lock_irqsave(&pcp->lock, UP_flags);
			}
			if (unlikely(pcp->flags & PCPF_CPU_DEAD)) {
				spin_unlock_irqrestore(&pcp->lock, UP_flags);
				pcp = NULL;
				free_one_page(zone, &folio->page, pfn,
					      order, FPI_NONE);
				continue;
			}
			locked_zone = zone;
			locked_cpu = cache_cpu;
		}

		/*
		 * Non-isolated types over MIGRATE_PCPTYPES get added
		 * to the MIGRATE_MOVABLE pcp list.
		 */
		if (unlikely(migratetype >= MIGRATE_PCPTYPES))
			migratetype = MIGRATE_MOVABLE;

		trace_mm_page_free_batched(&folio->page);
		free_frozen_page_commit(zone, pcp, &folio->page,
				migratetype, order, FPI_NONE,
				cache_cpu == owner_cpu);
	}

	if (pcp)
		spin_unlock_irqrestore(&pcp->lock, UP_flags);
	folio_batch_reinit(folios);
}

static void __split_page(struct page *page, unsigned int order)
{
	VM_WARN_ON_PAGE(PageCompound(page), page);

	split_page_owner(page, order, 0);
	pgalloc_tag_split(page_folio(page), order, 0);
	split_page_memcg(page, order);
}

/*
 * split_page takes a non-compound higher-order page, and splits it into
 * n (1<<order) sub-pages: page[0..n]
 * Each sub-page must be freed individually.
 *
 * Note: this is probably too low level an operation for use in drivers.
 * Please consult with lkml before using this in your driver.
 */
void split_page(struct page *page, unsigned int order)
{
	int i;

	VM_WARN_ON_PAGE(!page_count(page), page);

	for (i = 1; i < (1 << order); i++)
		set_page_refcounted(page + i);

	__split_page(page, order);
}
EXPORT_SYMBOL_GPL(split_page);

int __isolate_free_page(struct page *page, unsigned int order)
{
	struct zone *zone = page_zone(page);
	int mt = get_pageblock_migratetype(page);

	if (!is_migrate_isolate(mt)) {
		unsigned long watermark;
		/*
		 * Obey watermarks as if the page was being allocated. We can
		 * emulate a high-order watermark check with a raised order-0
		 * watermark, because we already know our high-order page
		 * exists.
		 */
		watermark = zone->_watermark[WMARK_MIN] + (1UL << order);
		if (!zone_watermark_ok(zone, 0, watermark, 0, ALLOC_CMA))
			return 0;
	}

	del_page_from_free_list(page, zone, order, mt);

	/*
	 * The free page is leaving buddy. For order >= pageblock_order, every
	 * constituent pageblock had PB_all_free set; clear those bits and
	 * decrement nr_free so the SPB pageblock-level counter stays in sync.
	 */
	if (order >= pageblock_order)
		superpageblock_range_now_used(page, order);

	/*
	 * Set the pageblock if the isolated page is at least half of a
	 * pageblock
	 */
	if (order >= pageblock_order - 1) {
		struct page *endpage = page + (1 << order) - 1;
		for (; page < endpage; page += pageblock_nr_pages) {
			int mt = get_pageblock_migratetype(page);
			/*
			 * Only change normal pageblocks (i.e., they can merge
			 * with others)
			 */
			if (migratetype_is_mergeable(mt))
				move_freepages_block(zone, page, mt,
						     MIGRATE_MOVABLE);
		}
	}

	return 1UL << order;
}

/**
 * __putback_isolated_page - Return a now-isolated page back where we got it
 * @page: Page that was isolated
 * @order: Order of the isolated page
 * @mt: The page's pageblock's migratetype
 *
 * This function is meant to return a page pulled from the free lists via
 * __isolate_free_page back to the free lists they were pulled from.
 */
void __putback_isolated_page(struct page *page, unsigned int order, int mt)
{
	struct zone *zone = page_zone(page);

	/* zone lock should be held when this function is called */
	lockdep_assert_held(&zone->lock);

	/* Return isolated page to tail of freelist. */
	__free_one_page(page, page_to_pfn(page), zone, order, mt,
			FPI_SKIP_REPORT_NOTIFY | FPI_TO_TAIL);
}

/*
 * Update NUMA hit/miss statistics
 */
static inline void zone_statistics(struct zone *preferred_zone, struct zone *z,
				   long nr_account)
{
#ifdef CONFIG_NUMA
	enum numa_stat_item local_stat = NUMA_LOCAL;

	/* skip numa counters update if numa stats is disabled */
	if (!static_branch_likely(&vm_numa_stat_key))
		return;

	if (zone_to_nid(z) != numa_node_id())
		local_stat = NUMA_OTHER;

	if (zone_to_nid(z) == zone_to_nid(preferred_zone))
		__count_numa_events(z, NUMA_HIT, nr_account);
	else {
		__count_numa_events(z, NUMA_MISS, nr_account);
		__count_numa_events(preferred_zone, NUMA_FOREIGN, nr_account);
	}
	__count_numa_events(z, local_stat, nr_account);
#endif
}

static __always_inline
struct page *rmqueue_buddy(struct zone *preferred_zone, struct zone *zone,
			   unsigned int order, unsigned int alloc_flags,
			   int migratetype)
{
	struct page *page;
	unsigned long flags;

	do {
		page = NULL;
		if (unlikely(alloc_flags & ALLOC_TRYLOCK)) {
			if (!spin_trylock_irqsave(&zone->lock, flags))
				return NULL;
		} else {
			spin_lock_irqsave(&zone->lock, flags);
		}
		if (alloc_flags & ALLOC_HIGHATOMIC)
			page = __rmqueue_smallest(zone, order,
						  MIGRATE_HIGHATOMIC,
						  alloc_flags, NULL);
		if (!page) {
			enum rmqueue_mode rmqm = RMQUEUE_NORMAL;

			page = __rmqueue(zone, order, migratetype, alloc_flags, &rmqm);

			/*
			 * If the allocation fails, allow OOM handling and
			 * order-0 (atomic) allocs access to HIGHATOMIC
			 * reserves as failing now is worse than failing a
			 * high-order atomic allocation in the future.
			 */
			if (!page && (alloc_flags & (ALLOC_OOM|ALLOC_NON_BLOCK)))
				page = __rmqueue_smallest(zone, order,
							  MIGRATE_HIGHATOMIC,
							  alloc_flags, NULL);

			if (!page) {
				spin_unlock_irqrestore(&zone->lock, flags);
				return NULL;
			}
		}
		spin_unlock_irqrestore(&zone->lock, flags);
	} while (check_new_pages(page, order));

	__count_zid_vm_events(PGALLOC, page_zonenum(page), 1 << order);
	zone_statistics(preferred_zone, zone, 1);

	return page;
}

static int nr_pcp_alloc(struct per_cpu_pages *pcp, struct zone *zone, int order)
{
	int high, base_batch, batch, max_nr_alloc;
	int high_max, high_min;

	base_batch = READ_ONCE(pcp->batch);
	high_min = READ_ONCE(pcp->high_min);
	high_max = READ_ONCE(pcp->high_max);
	high = pcp->high = clamp(pcp->high, high_min, high_max);

	/* Check for PCP disabled or boot pageset */
	if (unlikely(high < base_batch))
		return 1;

	if (order)
		batch = base_batch;
	else
		batch = (base_batch << pcp->alloc_factor);

	/*
	 * If we had larger pcp->high, we could avoid to allocate from
	 * zone.
	 */
	if (high_min != high_max && !test_bit(ZONE_BELOW_HIGH, &zone->flags))
		high = pcp->high = min(high + batch, high_max);

	if (!order) {
		max_nr_alloc = max(high - pcp->count - base_batch, base_batch);
		/*
		 * Double the number of pages allocated each time there is
		 * subsequent allocation of order-0 pages without any freeing.
		 */
		if (batch <= max_nr_alloc &&
		    pcp->alloc_factor < CONFIG_PCP_BATCH_SCALE_MAX)
			pcp->alloc_factor++;
		batch = min(batch, max_nr_alloc);
	}

	/*
	 * Scale batch relative to order if batch implies free pages
	 * can be stored on the PCP. Batch can be 1 for small zones or
	 * for boot pagesets which should never store free pages as
	 * the pages may belong to arbitrary zones.
	 */
	if (batch > 1)
		batch = max(batch >> order, 2);

	return batch;
}

/* Remove page from the per-cpu list, caller must protect the list */
static inline
struct page *__rmqueue_pcplist(struct zone *zone, unsigned int order,
			int migratetype,
			unsigned int alloc_flags,
			struct per_cpu_pages *pcp)
{
	struct page *page;

	do {
		/* Try to find/split from existing PCP stock */
		page = pcp_rmqueue_smallest(pcp, migratetype, order);
		if (!page) {
			int batch = nr_pcp_alloc(pcp, zone, order);

			if (!rmqueue_bulk(zone, order, batch, migratetype,
					  alloc_flags, pcp))
				return NULL;

			page = pcp_rmqueue_smallest(pcp, migratetype, order);
			if (unlikely(!page))
				return NULL;
		}
	} while (check_new_pages(page, order));

	return page;
}

/* Lock and remove page from the per-cpu list */
static struct page *rmqueue_pcplist(struct zone *preferred_zone,
			struct zone *zone, unsigned int order,
			int migratetype, unsigned int alloc_flags)
{
	struct per_cpu_pages *pcp;
	struct page *page;
	unsigned long UP_flags;

	/* spin_trylock may fail due to a parallel drain or IRQ reentrancy. */
	pcp = pcp_spin_trylock(zone->per_cpu_pageset, UP_flags);
	if (!pcp)
		return NULL;

	/*
	 * On allocation, reduce the number of pages that are batch freed.
	 * See nr_pcp_free() where free_factor is increased for subsequent
	 * frees.
	 */
	pcp->free_count >>= 1;
	page = __rmqueue_pcplist(zone, order, migratetype, alloc_flags, pcp);
	pcp_spin_unlock(pcp, UP_flags);
	if (page) {
		__count_zid_vm_events(PGALLOC, page_zonenum(page), 1 << order);
		zone_statistics(preferred_zone, zone, 1);
	}
	return page;
}

/*
 * Allocate a page from the given zone.
 * Use pcplists for THP or "cheap" high-order allocations.
 */

/*
 * Do not instrument rmqueue() with KMSAN. This function may call
 * __msan_poison_alloca() through a call to set_pfnblock_migratetype().
 * If __msan_poison_alloca() attempts to allocate pages for the stack depot, it
 * may call rmqueue() again, which will result in a deadlock.
 */
__no_sanitize_memory
static inline
struct page *rmqueue(struct zone *preferred_zone,
			struct zone *zone, unsigned int order,
			gfp_t gfp_flags, unsigned int alloc_flags,
			int migratetype)
{
	struct page *page;

	if (likely(pcp_allowed_order(order))) {
		page = rmqueue_pcplist(preferred_zone, zone, order,
				       migratetype, alloc_flags);
		if (likely(page))
			goto out;
	}

	page = rmqueue_buddy(preferred_zone, zone, order, alloc_flags,
							migratetype);

out:
	VM_BUG_ON_PAGE(page && bad_range(zone, page), page);
	return page;
}

/*
 * Reserve the pageblock(s) surrounding an allocation request for
 * exclusive use of high-order atomic allocations if there are no
 * empty page blocks that contain a page with a suitable order
 */
static void reserve_highatomic_pageblock(struct page *page, int order,
					 struct zone *zone)
{
	int mt;
	unsigned long max_managed, flags;

	/*
	 * The number reserved as: minimum is 1 pageblock, maximum is
	 * roughly 1% of a zone. But if 1% of a zone falls below a
	 * pageblock size, then don't reserve any pageblocks.
	 * Check is race-prone but harmless.
	 */
	if ((zone_managed_pages(zone) / 100) < pageblock_nr_pages)
		return;
	max_managed = ALIGN((zone_managed_pages(zone) / 100), pageblock_nr_pages);
	if (zone->nr_reserved_highatomic >= max_managed)
		return;

	spin_lock_irqsave(&zone->lock, flags);

	/* Recheck the nr_reserved_highatomic limit under the lock */
	if (zone->nr_reserved_highatomic >= max_managed)
		goto out_unlock;

	/* Yoink! */
	mt = get_pageblock_migratetype(page);
	/* Only reserve normal pageblocks (i.e., they can merge with others) */
	if (!migratetype_is_mergeable(mt))
		goto out_unlock;

	if (order < pageblock_order) {
		if (move_freepages_block(zone, page, mt, MIGRATE_HIGHATOMIC) == -1)
			goto out_unlock;
		zone->nr_reserved_highatomic += pageblock_nr_pages;
	} else {
		change_pageblock_range(page, order, MIGRATE_HIGHATOMIC);
		zone->nr_reserved_highatomic += 1 << order;
	}

out_unlock:
	spin_unlock_irqrestore(&zone->lock, flags);
}

/*
 * Used when an allocation is about to fail under memory pressure. This
 * potentially hurts the reliability of high-order allocations when under
 * intense memory pressure but failed atomic allocations should be easier
 * to recover from than an OOM.
 *
 * If @force is true, try to unreserve pageblocks even though highatomic
 * pageblock is exhausted.
 */
static bool unreserve_highatomic_pageblock(const struct alloc_context *ac,
						bool force)
{
	struct zonelist *zonelist = ac->zonelist;
	unsigned long flags;
	struct zoneref *z;
	struct zone *zone;
	struct page *page;
	int order;
	int ret;

	for_each_zone_zonelist_nodemask(zone, z, zonelist, ac->highest_zoneidx,
								ac->nodemask) {
		/*
		 * Preserve at least one pageblock unless memory pressure
		 * is really high.
		 */
		if (!force && zone->nr_reserved_highatomic <=
					pageblock_nr_pages)
			continue;

		spin_lock_irqsave(&zone->lock, flags);
		for (order = 0; order < NR_PAGE_ORDERS; order++) {
			struct free_area *area;
			struct superpageblock *sb;
			unsigned long size;
			unsigned long i;

			page = NULL;
			/* Search per-superpageblock free lists */
			for (i = 0; i < zone->nr_superpageblocks && !page; i++) {
				sb = &zone->superpageblocks[i];
				area = &sb->free_area[order];
				page = get_page_from_free_area(area,
							       MIGRATE_HIGHATOMIC);
			}
			if (!page)
				continue;

			size = max(pageblock_nr_pages, 1UL << order);
			/*
			 * It should never happen but changes to
			 * locking could inadvertently allow a per-cpu
			 * drain to add pages to MIGRATE_HIGHATOMIC
			 * while unreserving so be safe and watch for
			 * underflows.
			 */
			if (WARN_ON_ONCE(size > zone->nr_reserved_highatomic))
				size = zone->nr_reserved_highatomic;
			zone->nr_reserved_highatomic -= size;

			/*
			 * Convert to ac->migratetype and avoid the normal
			 * pageblock stealing heuristics. Minimally, the caller
			 * is doing the work and needs the pages. More
			 * importantly, if the block was always converted to
			 * MIGRATE_UNMOVABLE or another type then the number
			 * of pageblocks that cannot be completely freed
			 * may increase.
			 */
			if (order < pageblock_order)
				ret = move_freepages_block(zone, page,
							   MIGRATE_HIGHATOMIC,
							   ac->migratetype);
			else {
				move_to_free_list(page, zone, order,
						  MIGRATE_HIGHATOMIC,
						  ac->migratetype);
				change_pageblock_range(page, order,
						       ac->migratetype);
				ret = 1;
			}
			/*
			 * Reserving the block(s) already succeeded,
			 * so this should not fail on zone boundaries.
			 */
			WARN_ON_ONCE(ret == -1);
			if (ret > 0) {
				spin_unlock_irqrestore(&zone->lock, flags);
				return ret;
			}
		}
		spin_unlock_irqrestore(&zone->lock, flags);
	}

	return false;
}

static inline long __zone_watermark_unusable_free(struct zone *z,
				unsigned int order, unsigned int alloc_flags)
{
	long unusable_free = (1 << order) - 1;

	/*
	 * If the caller does not have rights to reserves below the min
	 * watermark then subtract the free pages reserved for highatomic.
	 */
	if (likely(!(alloc_flags & ALLOC_RESERVES)))
		unusable_free += READ_ONCE(z->nr_free_highatomic);

#ifdef CONFIG_CMA
	/* If allocation can't use CMA areas don't use free CMA pages */
	if (!(alloc_flags & ALLOC_CMA))
		unusable_free += zone_page_state(z, NR_FREE_CMA_PAGES);
#endif

	return unusable_free;
}

/*
 * Return true if free base pages are above 'mark'. For high-order checks it
 * will return true of the order-0 watermark is reached and there is at least
 * one free page of a suitable size. Checking now avoids taking the zone lock
 * to check in the allocation paths if no pages are free.
 */
bool __zone_watermark_ok(struct zone *z, unsigned int order, unsigned long mark,
			 int highest_zoneidx, unsigned int alloc_flags,
			 long free_pages)
{
	long min = mark;
	int o;

	/* free_pages may go negative - that's OK */
	free_pages -= __zone_watermark_unusable_free(z, order, alloc_flags);

	if (unlikely(alloc_flags & ALLOC_RESERVES)) {
		/*
		 * __GFP_HIGH allows access to 50% of the min reserve as well
		 * as OOM.
		 */
		if (alloc_flags & ALLOC_MIN_RESERVE) {
			min -= min / 2;

			/*
			 * Non-blocking allocations (e.g. GFP_ATOMIC) can
			 * access more reserves than just __GFP_HIGH. Other
			 * non-blocking allocations requests such as GFP_NOWAIT
			 * or (GFP_KERNEL & ~__GFP_DIRECT_RECLAIM) do not get
			 * access to the min reserve.
			 */
			if (alloc_flags & ALLOC_NON_BLOCK)
				min -= min / 4;
		}

		/*
		 * OOM victims can try even harder than the normal reserve
		 * users on the grounds that it's definitely going to be in
		 * the exit path shortly and free memory. Any allocation it
		 * makes during the free path will be small and short-lived.
		 */
		if (alloc_flags & ALLOC_OOM)
			min -= min / 2;
	}

	/*
	 * Check watermarks for an order-0 allocation request. If these
	 * are not met, then a high-order request also cannot go ahead
	 * even if a suitable page happened to be free.
	 */
	if (free_pages <= min + z->lowmem_reserve[highest_zoneidx])
		return false;

	/* If this is an order-0 request then the watermark is fine */
	if (!order)
		return true;

	/*
	 * For a high-order request, check at least one suitable page is free.
	 * Zone free_area nr_free is shadowed — it includes pages on
	 * per-superpageblock free lists. A non-zero nr_free means the allocator
	 * will find pages on superpageblock lists even if zone list heads are
	 * empty.
	 */
	for (o = order; o < NR_PAGE_ORDERS; o++) {
		struct free_area *area = &z->free_area[o];

		if (!area->nr_free)
			continue;

		return true;
	}
	return false;
}

bool zone_watermark_ok(struct zone *z, unsigned int order, unsigned long mark,
		      int highest_zoneidx, unsigned int alloc_flags)
{
	return __zone_watermark_ok(z, order, mark, highest_zoneidx, alloc_flags,
					zone_page_state(z, NR_FREE_PAGES));
}

static inline bool zone_watermark_fast(struct zone *z, unsigned int order,
				unsigned long mark, int highest_zoneidx,
				unsigned int alloc_flags, gfp_t gfp_mask)
{
	long free_pages;

	free_pages = zone_page_state(z, NR_FREE_PAGES);

	/*
	 * Fast check for order-0 only. If this fails then the reserves
	 * need to be calculated.
	 */
	if (!order) {
		long usable_free;
		long reserved;

		usable_free = free_pages;
		reserved = __zone_watermark_unusable_free(z, 0, alloc_flags);

		/* reserved may over estimate high-atomic reserves. */
		usable_free -= min(usable_free, reserved);
		if (usable_free > mark + z->lowmem_reserve[highest_zoneidx])
			return true;
	}

	return __zone_watermark_ok(z, order, mark, highest_zoneidx, alloc_flags,
					free_pages);
}

#ifdef CONFIG_NUMA
int __read_mostly node_reclaim_distance = RECLAIM_DISTANCE;

static bool zone_allows_reclaim(struct zone *local_zone, struct zone *zone)
{
	return node_distance(zone_to_nid(local_zone), zone_to_nid(zone)) <=
				node_reclaim_distance;
}
#else	/* CONFIG_NUMA */
static bool zone_allows_reclaim(struct zone *local_zone, struct zone *zone)
{
	return true;
}
#endif	/* CONFIG_NUMA */

/*
 * The restriction on ZONE_DMA32 as being a suitable zone to use to avoid
 * fragmentation is subtle. If the preferred zone was HIGHMEM then
 * premature use of a lower zone may cause lowmem pressure problems that
 * are worse than fragmentation. If the next zone is ZONE_DMA then it is
 * probably too small. It only makes sense to spread allocations to avoid
 * fragmentation between the Normal and DMA32 zones.
 */
static inline unsigned int
alloc_flags_nofragment(struct zone *zone, gfp_t gfp_mask)
{
	unsigned int alloc_flags;

	/*
	 * __GFP_KSWAPD_RECLAIM is assumed to be the same as ALLOC_KSWAPD
	 * to save a branch.
	 */
	alloc_flags = (__force int) (gfp_mask & __GFP_KSWAPD_RECLAIM);

	if (defrag_mode) {
		alloc_flags |= ALLOC_NOFRAGMENT;
		return alloc_flags;
	}

#ifdef CONFIG_ZONE_DMA32
	if (!zone)
		return alloc_flags;

	if (zone_idx(zone) != ZONE_NORMAL)
		return alloc_flags;

	/*
	 * If ZONE_DMA32 exists, assume it is the one after ZONE_NORMAL and
	 * the pointer is within zone->zone_pgdat->node_zones[]. Also assume
	 * on UMA that if Normal is populated then so is DMA32.
	 */
	BUILD_BUG_ON(ZONE_NORMAL - ZONE_DMA32 != 1);
	if (nr_online_nodes > 1 && !populated_zone(--zone))
		return alloc_flags;

	alloc_flags |= ALLOC_NOFRAGMENT;
#endif /* CONFIG_ZONE_DMA32 */
	return alloc_flags;
}

/* Must be called after current_gfp_context() which can change gfp_mask */
static inline unsigned int gfp_to_alloc_flags_cma(gfp_t gfp_mask,
						  unsigned int alloc_flags)
{
#ifdef CONFIG_CMA
	if (gfp_migratetype(gfp_mask) == MIGRATE_MOVABLE)
		alloc_flags |= ALLOC_CMA;
#endif
	return alloc_flags;
}

/*
 * get_page_from_freelist goes through the zonelist trying to allocate
 * a page.
 */
static struct page *
get_page_from_freelist(gfp_t gfp_mask, unsigned int order, int alloc_flags,
						const struct alloc_context *ac)
{
	struct zoneref *z;
	struct zone *zone;
	struct pglist_data *last_pgdat = NULL;
	bool last_pgdat_dirty_ok = false;
	bool no_fallback;
	bool skip_kswapd_nodes = nr_online_nodes > 1;
	bool skipped_kswapd_nodes = false;

retry:
	/*
	 * Scan zonelist, looking for a zone with enough free.
	 * See also cpuset_current_node_allowed() comment in kernel/cgroup/cpuset.c.
	 */
	no_fallback = alloc_flags & ALLOC_NOFRAGMENT;
	z = ac->preferred_zoneref;
	for_next_zone_zonelist_nodemask(zone, z, ac->highest_zoneidx,
					ac->nodemask) {
		struct page *page;
		unsigned long mark;

		if (cpusets_enabled() &&
			(alloc_flags & ALLOC_CPUSET) &&
			!__cpuset_zone_allowed(zone, gfp_mask))
				continue;
		/*
		 * When allocating a page cache page for writing, we
		 * want to get it from a node that is within its dirty
		 * limit, such that no single node holds more than its
		 * proportional share of globally allowed dirty pages.
		 * The dirty limits take into account the node's
		 * lowmem reserves and high watermark so that kswapd
		 * should be able to balance it without having to
		 * write pages from its LRU list.
		 *
		 * XXX: For now, allow allocations to potentially
		 * exceed the per-node dirty limit in the slowpath
		 * (spread_dirty_pages unset) before going into reclaim,
		 * which is important when on a NUMA setup the allowed
		 * nodes are together not big enough to reach the
		 * global limit.  The proper fix for these situations
		 * will require awareness of nodes in the
		 * dirty-throttling and the flusher threads.
		 */
		if (ac->spread_dirty_pages) {
			if (last_pgdat != zone->zone_pgdat) {
				last_pgdat = zone->zone_pgdat;
				last_pgdat_dirty_ok = node_dirty_ok(zone->zone_pgdat);
			}

			if (!last_pgdat_dirty_ok)
				continue;
		}

		if (no_fallback && !defrag_mode && nr_online_nodes > 1 &&
		    zone != zonelist_zone(ac->preferred_zoneref)) {
			int local_nid;

			/*
			 * If moving to a remote node, retry but allow
			 * fragmenting fallbacks. Locality is more important
			 * than fragmentation avoidance.
			 */
			local_nid = zonelist_node_idx(ac->preferred_zoneref);
			if (zone_to_nid(zone) != local_nid) {
				alloc_flags &= ~ALLOC_NOFRAGMENT;
				goto retry;
			}
		}

		/*
		 * If kswapd is already active on a node, keep looking
		 * for other nodes that might be idle. This can happen
		 * if another process has NUMA bindings and is causing
		 * kswapd wakeups on only some nodes. Avoid accidental
		 * "node_reclaim_mode"-like behavior in this case.
		 */
		if (skip_kswapd_nodes &&
		    !waitqueue_active(&zone->zone_pgdat->kswapd_wait)) {
			skipped_kswapd_nodes = true;
			continue;
		}

		cond_accept_memory(zone, order, alloc_flags);

		/*
		 * Detect whether the number of free pages is below high
		 * watermark.  If so, we will decrease pcp->high and free
		 * PCP pages in free path to reduce the possibility of
		 * premature page reclaiming.  Detection is done here to
		 * avoid to do that in hotter free path.
		 */
		if (test_bit(ZONE_BELOW_HIGH, &zone->flags))
			goto check_alloc_wmark;

		mark = high_wmark_pages(zone);
		if (zone_watermark_fast(zone, order, mark,
					ac->highest_zoneidx, alloc_flags,
					gfp_mask))
			goto try_this_zone;
		else
			set_bit(ZONE_BELOW_HIGH, &zone->flags);

check_alloc_wmark:
		mark = wmark_pages(zone, alloc_flags & ALLOC_WMARK_MASK);
		if (!zone_watermark_fast(zone, order, mark,
				       ac->highest_zoneidx, alloc_flags,
				       gfp_mask)) {
			int ret;

			if (cond_accept_memory(zone, order, alloc_flags))
				goto try_this_zone;

			/*
			 * Watermark failed for this zone, but see if we can
			 * grow this zone if it contains deferred pages.
			 */
			if (deferred_pages_enabled()) {
				if (_deferred_grow_zone(zone, order))
					goto try_this_zone;
			}
			/* Checked here to keep the fast path fast */
			BUILD_BUG_ON(ALLOC_NO_WATERMARKS < NR_WMARK);
			if (alloc_flags & ALLOC_NO_WATERMARKS)
				goto try_this_zone;

			if (!node_reclaim_enabled() ||
			    !zone_allows_reclaim(zonelist_zone(ac->preferred_zoneref), zone))
				continue;

			ret = node_reclaim(zone->zone_pgdat, gfp_mask, order);
			switch (ret) {
			case NODE_RECLAIM_NOSCAN:
				/* did not scan */
				continue;
			case NODE_RECLAIM_FULL:
				/* scanned but unreclaimable */
				continue;
			default:
				/* did we reclaim enough */
				if (zone_watermark_ok(zone, order, mark,
					ac->highest_zoneidx, alloc_flags))
					goto try_this_zone;

				continue;
			}
		}

try_this_zone:
		page = rmqueue(zonelist_zone(ac->preferred_zoneref), zone, order,
				gfp_mask, alloc_flags, ac->migratetype);
		if (page) {
			prep_new_page(page, order, gfp_mask, alloc_flags);

			/*
			 * If this is a high-order atomic allocation then check
			 * if the pageblock should be reserved for the future
			 */
			if (unlikely(alloc_flags & ALLOC_HIGHATOMIC))
				reserve_highatomic_pageblock(page, order, zone);

			return page;
		} else {
			if (cond_accept_memory(zone, order, alloc_flags))
				goto try_this_zone;

			/* Try again if zone has deferred pages */
			if (deferred_pages_enabled()) {
				if (_deferred_grow_zone(zone, order))
					goto try_this_zone;
			}
		}
	}

	/*
	 * If we skipped over nodes with active kswapds and found no
	 * idle nodes, retry and place anywhere the watermarks permit.
	 */
	if (skip_kswapd_nodes && skipped_kswapd_nodes) {
		skip_kswapd_nodes = false;
		goto retry;
	}

	/*
	 * It's possible on a UMA machine to get through all zones that are
	 * fragmented. If avoiding fragmentation, reset and try again.
	 *
	 * For allocations that can do direct reclaim, keep NOFRAGMENT set
	 * and let the slowpath try reclaim and compaction to free pages in
	 * already-tainted superpageblocks before allowing clean SPBs to be
	 * tainted.
	 *
	 * Atomic allocations cannot reclaim, but try an intermediate step
	 * first: allow steal/claim from tainted SPBs only. This avoids
	 * tainting clean SPBs while still finding pages in tainted ones.
	 * Only drop NOFRAGMENT entirely if that also fails.
	 *
	 * Exception: callers that explicitly opted into failure with
	 * __GFP_NORETRY have a fallback path of their own (a smaller
	 * order, a different cache, returning NULL from a best-effort
	 * cache refill, etc.). Tainting a clean superpageblock is a
	 * lasting cost that outlives this allocation; it is not justified
	 * to absorb it just to satisfy a caller that already has a
	 * cheaper escape hatch. Return NULL and let the caller's fallback
	 * run instead.
	 */
	if (no_fallback && !defrag_mode &&
	    !(gfp_mask & __GFP_DIRECT_RECLAIM)) {
		struct zone *pref = zonelist_zone(ac->preferred_zoneref);

		if (gfp_mask & __GFP_NORETRY) {
			trace_spb_alloc_atomic_relax(pref, order,
				ac->migratetype, gfp_mask,
				SPB_ATOMIC_RELAX_NORETRY_SKIP);
			return NULL;
		}
		if (!(alloc_flags & ALLOC_NOFRAG_TAINTED_OK)) {
			trace_spb_alloc_atomic_relax(pref, order,
				ac->migratetype, gfp_mask,
				SPB_ATOMIC_RELAX_ADD_TAINTED_OK);
			alloc_flags |= ALLOC_NOFRAG_TAINTED_OK;
			goto retry;
		}
		trace_spb_alloc_atomic_relax(pref, order,
			ac->migratetype, gfp_mask,
			SPB_ATOMIC_RELAX_DROP_NOFRAGMENT);
		alloc_flags &= ~(ALLOC_NOFRAGMENT | ALLOC_NOFRAG_TAINTED_OK);
		goto retry;
	}

	return NULL;
}

static void warn_alloc_show_mem(gfp_t gfp_mask, nodemask_t *nodemask)
{
	unsigned int filter = SHOW_MEM_FILTER_NODES;

	/*
	 * This documents exceptions given to allocations in certain
	 * contexts that are allowed to allocate outside current's set
	 * of allowed nodes.
	 */
	if (!(gfp_mask & __GFP_NOMEMALLOC))
		if (tsk_is_oom_victim(current) ||
		    (current->flags & (PF_MEMALLOC | PF_EXITING)))
			filter &= ~SHOW_MEM_FILTER_NODES;
	if (!in_task() || !(gfp_mask & __GFP_DIRECT_RECLAIM))
		filter &= ~SHOW_MEM_FILTER_NODES;

	__show_mem(filter, nodemask, gfp_zone(gfp_mask));
	mem_cgroup_show_protected_memory(NULL);
}

void warn_alloc(gfp_t gfp_mask, nodemask_t *nodemask, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	static DEFINE_RATELIMIT_STATE(nopage_rs, 10*HZ, 1);

	if ((gfp_mask & __GFP_NOWARN) ||
	     !__ratelimit(&nopage_rs) ||
	     ((gfp_mask & __GFP_DMA) && !has_managed_dma()))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_warn("%s: %pV, mode:%#x(%pGg), nodemask=%*pbl",
			current->comm, &vaf, gfp_mask, &gfp_mask,
			nodemask_pr_args(nodemask));
	va_end(args);

	cpuset_print_current_mems_allowed();
	pr_cont("\n");
	dump_stack();
	warn_alloc_show_mem(gfp_mask, nodemask);
}

static inline struct page *
__alloc_pages_cpuset_fallback(gfp_t gfp_mask, unsigned int order,
			      unsigned int alloc_flags,
			      const struct alloc_context *ac)
{
	struct page *page;

	page = get_page_from_freelist(gfp_mask, order,
			alloc_flags|ALLOC_CPUSET, ac);
	/*
	 * fallback to ignore cpuset restriction if our nodes
	 * are depleted
	 */
	if (!page)
		page = get_page_from_freelist(gfp_mask, order,
				alloc_flags, ac);
	return page;
}

static inline struct page *
__alloc_pages_may_oom(gfp_t gfp_mask, unsigned int order,
	const struct alloc_context *ac, unsigned long *did_some_progress)
{
	struct oom_control oc = {
		.zonelist = ac->zonelist,
		.nodemask = ac->nodemask,
		.memcg = NULL,
		.gfp_mask = gfp_mask,
		.order = order,
	};
	struct page *page;

	*did_some_progress = 0;

	/*
	 * Acquire the oom lock.  If that fails, somebody else is
	 * making progress for us.
	 */
	if (!mutex_trylock(&oom_lock)) {
		*did_some_progress = 1;
		schedule_timeout_uninterruptible(1);
		return NULL;
	}

	/*
	 * Go through the zonelist yet one more time, keep very high watermark
	 * here, this is only to catch a parallel oom killing, we must fail if
	 * we're still under heavy pressure. But make sure that this reclaim
	 * attempt shall not depend on __GFP_DIRECT_RECLAIM && !__GFP_NORETRY
	 * allocation which will never fail due to oom_lock already held.
	 */
	page = get_page_from_freelist((gfp_mask | __GFP_HARDWALL) &
				      ~__GFP_DIRECT_RECLAIM, order,
				      ALLOC_WMARK_HIGH|ALLOC_CPUSET, ac);
	if (page)
		goto out;

	/* Coredumps can quickly deplete all memory reserves */
	if (current->flags & PF_DUMPCORE)
		goto out;
	/* The OOM killer will not help higher order allocs */
	if (order > PAGE_ALLOC_COSTLY_ORDER)
		goto out;
	/*
	 * We have already exhausted all our reclaim opportunities without any
	 * success so it is time to admit defeat. We will skip the OOM killer
	 * because it is very likely that the caller has a more reasonable
	 * fallback than shooting a random task.
	 *
	 * The OOM killer may not free memory on a specific node.
	 */
	if (gfp_mask & (__GFP_RETRY_MAYFAIL | __GFP_THISNODE))
		goto out;
	/* The OOM killer does not needlessly kill tasks for lowmem */
	if (ac->highest_zoneidx < ZONE_NORMAL)
		goto out;
	if (pm_suspended_storage())
		goto out;
	/*
	 * XXX: GFP_NOFS allocations should rather fail than rely on
	 * other request to make a forward progress.
	 * We are in an unfortunate situation where out_of_memory cannot
	 * do much for this context but let's try it to at least get
	 * access to memory reserved if the current task is killed (see
	 * out_of_memory). Once filesystems are ready to handle allocation
	 * failures more gracefully we should just bail out here.
	 */

	/* Exhausted what can be done so it's blame time */
	if (out_of_memory(&oc) ||
	    WARN_ON_ONCE_GFP(gfp_mask & __GFP_NOFAIL, gfp_mask)) {
		*did_some_progress = 1;

		/*
		 * Help non-failing allocations by giving them access to memory
		 * reserves
		 */
		if (gfp_mask & __GFP_NOFAIL)
			page = __alloc_pages_cpuset_fallback(gfp_mask, order,
					ALLOC_NO_WATERMARKS, ac);
	}
out:
	mutex_unlock(&oom_lock);
	return page;
}

/*
 * Maximum number of compaction retries with a progress before OOM
 * killer is consider as the only way to move forward.
 */
#define MAX_COMPACT_RETRIES 16

#ifdef CONFIG_COMPACTION
/* Try memory compaction for high-order allocations before reclaim */
static struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
		unsigned int alloc_flags, const struct alloc_context *ac,
		enum compact_priority prio, enum compact_result *compact_result)
{
	struct page *page = NULL;
	unsigned long pflags;
	unsigned int noreclaim_flag;

	if (!order)
		return NULL;

	/*
	 * Superpageblock-aware contiguous allocation for movable high-order
	 * allocations. Use superpageblock metadata to find clean ranges and
	 * evacuate them via alloc_contig_frozen_range, bypassing the
	 * blind compaction scanner entirely.
	 */
	if (order >= pageblock_order &&
	    ac->migratetype == MIGRATE_MOVABLE) {
		struct zoneref *z;
		struct zone *zone;

		for_each_zone_zonelist_nodemask(zone, z, ac->zonelist,
					       ac->highest_zoneidx,
					       ac->nodemask) {
			page = spb_try_alloc_contig(zone, 1UL << order,
						   gfp_mask);
			if (page) {
				prep_new_page(page, order, gfp_mask,
					      alloc_flags);
				*compact_result = COMPACT_SUCCESS;
				count_vm_event(COMPACTSUCCESS);
				return page;
			}
		}
	}

	/*
	 * Superpageblock-aware targeted evacuation for unmovable/reclaimable
	 * high-order allocations. Instead of blind compaction, find
	 * pageblocks of the right migratetype in tainted superpageblocks
	 * and evacuate their movable pages to create buddy coalescing
	 * opportunities.
	 */
	if (ac->migratetype == MIGRATE_UNMOVABLE ||
	    ac->migratetype == MIGRATE_RECLAIMABLE) {
		struct zoneref *z;
		struct zone *zone;

		for_each_zone_zonelist_nodemask(zone, z, ac->zonelist,
					       ac->highest_zoneidx,
					       ac->nodemask) {
			if (spb_evacuate_for_order(zone, order,
						  ac->migratetype)) {
				page = get_page_from_freelist(gfp_mask, order,
							     alloc_flags, ac);
				if (page) {
					*compact_result = COMPACT_SUCCESS;
					count_vm_event(COMPACTSUCCESS);
					return page;
				}
			}
		}
	}

	psi_memstall_enter(&pflags);
	delayacct_compact_start();
	noreclaim_flag = memalloc_noreclaim_save();

	*compact_result = try_to_compact_pages(gfp_mask, order, alloc_flags, ac,
								prio, &page);

	memalloc_noreclaim_restore(noreclaim_flag);
	psi_memstall_leave(&pflags);
	delayacct_compact_end();

	if (*compact_result == COMPACT_SKIPPED)
		return NULL;
	/*
	 * At least in one zone compaction wasn't deferred or skipped, so let's
	 * count a compaction stall
	 */
	count_vm_event(COMPACTSTALL);

	/* Prep a captured page if available */
	if (page)
		prep_new_page(page, order, gfp_mask, alloc_flags);

	/* Try get a page from the freelist if available */
	if (!page)
		page = get_page_from_freelist(gfp_mask, order, alloc_flags, ac);

	if (page) {
		struct zone *zone = page_zone(page);

		zone->compact_blockskip_flush = false;
		compaction_defer_reset(zone, order, true);
		count_vm_event(COMPACTSUCCESS);
		return page;
	}

	/*
	 * It's bad if compaction run occurs and fails. The most likely reason
	 * is that pages exist, but not enough to satisfy watermarks.
	 */
	count_vm_event(COMPACTFAIL);

	cond_resched();

	return NULL;
}

static inline bool
should_compact_retry(struct alloc_context *ac, int order, int alloc_flags,
		     enum compact_result compact_result,
		     enum compact_priority *compact_priority,
		     int *compaction_retries)
{
	int max_retries = MAX_COMPACT_RETRIES;
	int min_priority;
	bool ret = false;
	int retries = *compaction_retries;
	enum compact_priority priority = *compact_priority;

	if (!order)
		return false;

	if (fatal_signal_pending(current))
		return false;

	/*
	 * Compaction was skipped due to a lack of free order-0
	 * migration targets. Continue if reclaim can help.
	 */
	if (compact_result == COMPACT_SKIPPED) {
		ret = compaction_zonelist_suitable(ac, order, alloc_flags);
		goto out;
	}

	/*
	 * Compaction managed to coalesce some page blocks, but the
	 * allocation failed presumably due to a race. Retry some.
	 */
	if (compact_result == COMPACT_SUCCESS) {
		/*
		 * !costly requests are much more important than
		 * __GFP_RETRY_MAYFAIL costly ones because they are de
		 * facto nofail and invoke OOM killer to move on while
		 * costly can fail and users are ready to cope with
		 * that. 1/4 retries is rather arbitrary but we would
		 * need much more detailed feedback from compaction to
		 * make a better decision.
		 */
		if (order > PAGE_ALLOC_COSTLY_ORDER)
			max_retries /= 4;

		if (++(*compaction_retries) <= max_retries) {
			ret = true;
			goto out;
		}
	}

	/*
	 * Compaction failed. Retry with increasing priority.
	 */
	min_priority = (order > PAGE_ALLOC_COSTLY_ORDER) ?
			MIN_COMPACT_COSTLY_PRIORITY : MIN_COMPACT_PRIORITY;

	if (*compact_priority > min_priority) {
		(*compact_priority)--;
		*compaction_retries = 0;
		ret = true;
	}
out:
	trace_compact_retry(order, priority, compact_result, retries, max_retries, ret);
	return ret;
}
#else
static inline struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
		unsigned int alloc_flags, const struct alloc_context *ac,
		enum compact_priority prio, enum compact_result *compact_result)
{
	*compact_result = COMPACT_SKIPPED;
	return NULL;
}

static inline bool
should_compact_retry(struct alloc_context *ac, int order, int alloc_flags,
		     enum compact_result compact_result,
		     enum compact_priority *compact_priority,
		     int *compaction_retries)
{
	struct zone *zone;
	struct zoneref *z;

	if (!order || order > PAGE_ALLOC_COSTLY_ORDER)
		return false;

	/*
	 * There are setups with compaction disabled which would prefer to loop
	 * inside the allocator rather than hit the oom killer prematurely.
	 * Let's give them a good hope and keep retrying while the order-0
	 * watermarks are OK.
	 */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist,
				ac->highest_zoneidx, ac->nodemask) {
		if (zone_watermark_ok(zone, 0, min_wmark_pages(zone),
					ac->highest_zoneidx, alloc_flags))
			return true;
	}
	return false;
}
#endif /* CONFIG_COMPACTION */

#ifdef CONFIG_LOCKDEP
static struct lockdep_map __fs_reclaim_map =
	STATIC_LOCKDEP_MAP_INIT("fs_reclaim", &__fs_reclaim_map);

static bool __need_reclaim(gfp_t gfp_mask)
{
	/* no reclaim without waiting on it */
	if (!(gfp_mask & __GFP_DIRECT_RECLAIM))
		return false;

	/* this guy won't enter reclaim */
	if (current->flags & PF_MEMALLOC)
		return false;

	if (gfp_mask & __GFP_NOLOCKDEP)
		return false;

	return true;
}

void __fs_reclaim_acquire(unsigned long ip)
{
	lock_acquire_exclusive(&__fs_reclaim_map, 0, 0, NULL, ip);
}

void __fs_reclaim_release(unsigned long ip)
{
	lock_release(&__fs_reclaim_map, ip);
}

void fs_reclaim_acquire(gfp_t gfp_mask)
{
	gfp_mask = current_gfp_context(gfp_mask);

	if (__need_reclaim(gfp_mask)) {
		if (gfp_mask & __GFP_FS)
			__fs_reclaim_acquire(_RET_IP_);

#ifdef CONFIG_MMU_NOTIFIER
		lock_map_acquire(&__mmu_notifier_invalidate_range_start_map);
		lock_map_release(&__mmu_notifier_invalidate_range_start_map);
#endif

	}
}
EXPORT_SYMBOL_GPL(fs_reclaim_acquire);

void fs_reclaim_release(gfp_t gfp_mask)
{
	gfp_mask = current_gfp_context(gfp_mask);

	if (__need_reclaim(gfp_mask)) {
		if (gfp_mask & __GFP_FS)
			__fs_reclaim_release(_RET_IP_);
	}
}
EXPORT_SYMBOL_GPL(fs_reclaim_release);
#endif

/*
 * Zonelists may change due to hotplug during allocation. Detect when zonelists
 * have been rebuilt so allocation retries. Reader side does not lock and
 * retries the allocation if zonelist changes. Writer side is protected by the
 * embedded spin_lock.
 */
static DEFINE_SEQLOCK(zonelist_update_seq);

static unsigned int zonelist_iter_begin(void)
{
	if (IS_ENABLED(CONFIG_MEMORY_HOTREMOVE))
		return read_seqbegin(&zonelist_update_seq);

	return 0;
}

static unsigned int check_retry_zonelist(unsigned int seq)
{
	if (IS_ENABLED(CONFIG_MEMORY_HOTREMOVE))
		return read_seqretry(&zonelist_update_seq, seq);

	return seq;
}

/* Perform direct synchronous page reclaim */
static unsigned long
__perform_reclaim(gfp_t gfp_mask, unsigned int order,
					const struct alloc_context *ac)
{
	unsigned int noreclaim_flag;
	unsigned long progress;

	cond_resched();

	/* We now go into synchronous reclaim */
	cpuset_memory_pressure_bump();
	fs_reclaim_acquire(gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();

	progress = try_to_free_pages(ac->zonelist, order, gfp_mask,
								ac->nodemask);

	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(gfp_mask);

	cond_resched();

	return progress;
}

/* The really slow allocator path where we enter direct reclaim */
static inline struct page *
__alloc_pages_direct_reclaim(gfp_t gfp_mask, unsigned int order,
		unsigned int alloc_flags, const struct alloc_context *ac,
		unsigned long *did_some_progress)
{
	struct page *page = NULL;
	unsigned long pflags;
	bool drained = false;

	psi_memstall_enter(&pflags);
	*did_some_progress = __perform_reclaim(gfp_mask, order, ac);
	if (unlikely(!(*did_some_progress)))
		goto out;

retry:
	page = get_page_from_freelist(gfp_mask, order, alloc_flags, ac);

	/*
	 * If an allocation failed after direct reclaim, it could be because
	 * pages are pinned on the per-cpu lists or in high alloc reserves.
	 * Shrink them and try again
	 */
	if (!page && !drained) {
		unreserve_highatomic_pageblock(ac, false);
		drain_all_pages(NULL);
		drained = true;
		goto retry;
	}
out:
	psi_memstall_leave(&pflags);

	return page;
}

static void wake_all_kswapds(unsigned int order, gfp_t gfp_mask,
			     const struct alloc_context *ac)
{
	struct zoneref *z;
	struct zone *zone;
	pg_data_t *last_pgdat = NULL;
	enum zone_type highest_zoneidx = ac->highest_zoneidx;
	unsigned int reclaim_order;

	if (defrag_mode)
		reclaim_order = max(order, pageblock_order);
	else
		reclaim_order = order;

	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist, highest_zoneidx,
					ac->nodemask) {
		if (!managed_zone(zone))
			continue;
		if (last_pgdat == zone->zone_pgdat)
			continue;
		wakeup_kswapd(zone, gfp_mask, reclaim_order, highest_zoneidx);
		last_pgdat = zone->zone_pgdat;
	}
}

static inline unsigned int
gfp_to_alloc_flags(gfp_t gfp_mask, unsigned int order)
{
	unsigned int alloc_flags = ALLOC_WMARK_MIN | ALLOC_CPUSET;

	/*
	 * __GFP_HIGH is assumed to be the same as ALLOC_MIN_RESERVE
	 * and __GFP_KSWAPD_RECLAIM is assumed to be the same as ALLOC_KSWAPD
	 * to save two branches.
	 */
	BUILD_BUG_ON(__GFP_HIGH != (__force gfp_t) ALLOC_MIN_RESERVE);
	BUILD_BUG_ON(__GFP_KSWAPD_RECLAIM != (__force gfp_t) ALLOC_KSWAPD);

	/*
	 * The caller may dip into page reserves a bit more if the caller
	 * cannot run direct reclaim, or if the caller has realtime scheduling
	 * policy or is asking for __GFP_HIGH memory.  GFP_ATOMIC requests will
	 * set both ALLOC_NON_BLOCK and ALLOC_MIN_RESERVE(__GFP_HIGH).
	 */
	alloc_flags |= (__force int)
		(gfp_mask & (__GFP_HIGH | __GFP_KSWAPD_RECLAIM));

	if (!(gfp_mask & __GFP_DIRECT_RECLAIM)) {
		/*
		 * Not worth trying to allocate harder for __GFP_NOMEMALLOC even
		 * if it can't schedule.
		 */
		if (!(gfp_mask & __GFP_NOMEMALLOC)) {
			alloc_flags |= ALLOC_NON_BLOCK;

			if (order > 0 && (alloc_flags & ALLOC_MIN_RESERVE))
				alloc_flags |= ALLOC_HIGHATOMIC;
		}

		/*
		 * Ignore cpuset mems for non-blocking __GFP_HIGH (probably
		 * GFP_ATOMIC) rather than fail, see the comment for
		 * cpuset_current_node_allowed().
		 */
		if (alloc_flags & ALLOC_MIN_RESERVE)
			alloc_flags &= ~ALLOC_CPUSET;
	} else if (unlikely(rt_or_dl_task(current)) && in_task())
		alloc_flags |= ALLOC_MIN_RESERVE;

	alloc_flags = gfp_to_alloc_flags_cma(gfp_mask, alloc_flags);

	if (defrag_mode)
		alloc_flags |= ALLOC_NOFRAGMENT;

	/*
	 * Mark callers that have a cheap fallback if the page allocator returns
	 * NULL, so __rmqueue can refuse to taint a clean SPB when an existing
	 * tainted SPB still has free pageblocks waiting to be evacuated.
	 *
	 * Two shapes qualify:
	 *
	 *  1. Explicit fallback declaration: __GFP_NORETRY without
	 *     __GFP_RETRY_MAYFAIL. Used by THP, slab high-order refill,
	 *     skb_page_frag_refill on full sockets, etc.
	 *
	 *  2. Atomic-context shape: no __GFP_DIRECT_RECLAIM, no __GFP_NOMEMALLOC,
	 *     no __GFP_NOFAIL. These callers (GFP_ATOMIC, GFP_NOWAIT, including
	 *     ALLOC_HIGHATOMIC consumers) have implicit fallbacks: drop the
	 *     packet, demote the slab order, return ENOMEM up the slowpath,
	 *     retry from process context with GFP_KERNEL, etc. ALLOC_HIGHATOMIC
	 *     callers also get a second crack at the dedicated MIGRATE_HIGHATOMIC
	 *     reserve in rmqueue_buddy after __rmqueue returns NULL.
	 *     Tainting a 1 GiB SPB to satisfy any of them is a long-lived
	 *     fragmentation event for short-lived data.
	 *
	 * __GFP_MEMALLOC (reclaim recursion) and __GFP_NOFAIL (declared cannot
	 * fail) are excluded — they must succeed even at the cost of taint.
	 */
	if ((gfp_mask & __GFP_NORETRY) && !(gfp_mask & __GFP_RETRY_MAYFAIL))
		alloc_flags |= ALLOC_HIGHORDER_OPTIONAL;
	else if (!(gfp_mask & (__GFP_DIRECT_RECLAIM | __GFP_NOMEMALLOC |
			       __GFP_NOFAIL)))
		alloc_flags |= ALLOC_HIGHORDER_OPTIONAL;

	return alloc_flags;
}

static bool oom_reserves_allowed(struct task_struct *tsk)
{
	if (!tsk_is_oom_victim(tsk))
		return false;

	/*
	 * !MMU doesn't have oom reaper so give access to memory reserves
	 * only to the thread with TIF_MEMDIE set
	 */
	if (!IS_ENABLED(CONFIG_MMU) && !test_thread_flag(TIF_MEMDIE))
		return false;

	return true;
}

/*
 * Distinguish requests which really need access to full memory
 * reserves from oom victims which can live with a portion of it
 */
static inline int __gfp_pfmemalloc_flags(gfp_t gfp_mask)
{
	if (unlikely(gfp_mask & __GFP_NOMEMALLOC))
		return 0;
	if (gfp_mask & __GFP_MEMALLOC)
		return ALLOC_NO_WATERMARKS;
	if (in_serving_softirq() && (current->flags & PF_MEMALLOC))
		return ALLOC_NO_WATERMARKS;
	if (!in_interrupt()) {
		if (current->flags & PF_MEMALLOC)
			return ALLOC_NO_WATERMARKS;
		else if (oom_reserves_allowed(current))
			return ALLOC_OOM;
	}

	return 0;
}

bool gfp_pfmemalloc_allowed(gfp_t gfp_mask)
{
	return !!__gfp_pfmemalloc_flags(gfp_mask);
}

/*
 * Checks whether it makes sense to retry the reclaim to make a forward progress
 * for the given allocation request.
 *
 * We give up when we either have tried MAX_RECLAIM_RETRIES in a row
 * without success, or when we couldn't even meet the watermark if we
 * reclaimed all remaining pages on the LRU lists.
 *
 * Returns true if a retry is viable or false to enter the oom path.
 */
static inline bool
should_reclaim_retry(gfp_t gfp_mask, unsigned order,
		     struct alloc_context *ac, int alloc_flags,
		     bool did_some_progress, int *no_progress_loops)
{
	struct zone *zone;
	struct zoneref *z;
	bool ret = false;

	/*
	 * Costly allocations might have made a progress but this doesn't mean
	 * their order will become available due to high fragmentation so
	 * always increment the no progress counter for them
	 */
	if (did_some_progress && order <= PAGE_ALLOC_COSTLY_ORDER)
		*no_progress_loops = 0;
	else
		(*no_progress_loops)++;

	if (*no_progress_loops > MAX_RECLAIM_RETRIES)
		goto out;


	/*
	 * Keep reclaiming pages while there is a chance this will lead
	 * somewhere.  If none of the target zones can satisfy our allocation
	 * request even if all reclaimable pages are considered then we are
	 * screwed and have to go OOM.
	 */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist,
				ac->highest_zoneidx, ac->nodemask) {
		unsigned long available;
		unsigned long reclaimable;
		unsigned long min_wmark = min_wmark_pages(zone);
		bool wmark;

		if (cpusets_enabled() &&
			(alloc_flags & ALLOC_CPUSET) &&
			!__cpuset_zone_allowed(zone, gfp_mask))
				continue;

		available = reclaimable = zone_reclaimable_pages(zone);
		available += zone_page_state_snapshot(zone, NR_FREE_PAGES);

		/*
		 * Would the allocation succeed if we reclaimed all
		 * reclaimable pages?
		 */
		wmark = __zone_watermark_ok(zone, order, min_wmark,
				ac->highest_zoneidx, alloc_flags, available);
		trace_reclaim_retry_zone(z, order, reclaimable,
				available, min_wmark, *no_progress_loops, wmark);
		if (wmark) {
			ret = true;
			break;
		}
	}

	/*
	 * Memory allocation/reclaim might be called from a WQ context and the
	 * current implementation of the WQ concurrency control doesn't
	 * recognize that a particular WQ is congested if the worker thread is
	 * looping without ever sleeping. Therefore we have to do a short sleep
	 * here rather than calling cond_resched().
	 */
	if (current->flags & PF_WQ_WORKER)
		schedule_timeout_uninterruptible(1);
	else
		cond_resched();
out:
	/* Before OOM, exhaust highatomic_reserve */
	if (!ret)
		return unreserve_highatomic_pageblock(ac, true);

	return ret;
}

static inline bool
check_retry_cpuset(int cpuset_mems_cookie, struct alloc_context *ac)
{
	/*
	 * It's possible that cpuset's mems_allowed and the nodemask from
	 * mempolicy don't intersect. This should be normally dealt with by
	 * policy_nodemask(), but it's possible to race with cpuset update in
	 * such a way the check therein was true, and then it became false
	 * before we got our cpuset_mems_cookie here.
	 * This assumes that for all allocations, ac->nodemask can come only
	 * from MPOL_BIND mempolicy (whose documented semantics is to be ignored
	 * when it does not intersect with the cpuset restrictions) or the
	 * caller can deal with a violated nodemask.
	 */
	if (cpusets_enabled() && ac->nodemask &&
			!cpuset_nodemask_valid_mems_allowed(ac->nodemask)) {
		ac->nodemask = NULL;
		return true;
	}

	/*
	 * When updating a task's mems_allowed or mempolicy nodemask, it is
	 * possible to race with parallel threads in such a way that our
	 * allocation can fail while the mask is being updated. If we are about
	 * to fail, check if the cpuset changed during allocation and if so,
	 * retry.
	 */
	if (read_mems_allowed_retry(cpuset_mems_cookie))
		return true;

	return false;
}

static inline struct page *
__alloc_pages_slowpath(gfp_t gfp_mask, unsigned int order,
						struct alloc_context *ac)
{
	bool can_direct_reclaim = gfp_mask & __GFP_DIRECT_RECLAIM;
	bool can_compact = can_direct_reclaim && gfp_compaction_allowed(gfp_mask);
	bool nofail = gfp_mask & __GFP_NOFAIL;
	const bool costly_order = order > PAGE_ALLOC_COSTLY_ORDER;
	struct page *page = NULL;
	unsigned int alloc_flags;
	unsigned long did_some_progress;
	enum compact_priority compact_priority;
	enum compact_result compact_result;
	int compaction_retries;
	int no_progress_loops;
	unsigned int cpuset_mems_cookie;
	unsigned int zonelist_iter_cookie;
	int reserve_flags;
	bool compact_first = false;
	bool can_retry_reserves = true;

	if (unlikely(nofail)) {
		/*
		 * Also we don't support __GFP_NOFAIL without __GFP_DIRECT_RECLAIM,
		 * otherwise, we may result in lockup.
		 */
		WARN_ON_ONCE(!can_direct_reclaim);
		/*
		 * PF_MEMALLOC request from this context is rather bizarre
		 * because we cannot reclaim anything and only can loop waiting
		 * for somebody to do a work for us.
		 */
		WARN_ON_ONCE(current->flags & PF_MEMALLOC);
	}

restart:
	compaction_retries = 0;
	no_progress_loops = 0;
	compact_result = COMPACT_SKIPPED;
	compact_priority = DEF_COMPACT_PRIORITY;
	cpuset_mems_cookie = read_mems_allowed_begin();
	zonelist_iter_cookie = zonelist_iter_begin();

	/*
	 * For costly allocations, try direct compaction first, as it's likely
	 * that we have enough base pages and don't need to reclaim. For non-
	 * movable high-order allocations, do that as well, as compaction will
	 * try prevent permanent fragmentation by migrating from blocks of the
	 * same migratetype.
	 */
	if (can_compact && (costly_order || (order > 0 &&
					ac->migratetype != MIGRATE_MOVABLE))) {
		compact_first = true;
		compact_priority = INIT_COMPACT_PRIORITY;
	}

	/*
	 * The fast path uses conservative alloc_flags to succeed only until
	 * kswapd needs to be woken up, and to avoid the cost of setting up
	 * alloc_flags precisely. So we do that now.
	 */
	alloc_flags = gfp_to_alloc_flags(gfp_mask, order);

	/*
	 * We need to recalculate the starting point for the zonelist iterator
	 * because we might have used different nodemask in the fast path, or
	 * there was a cpuset modification and we are retrying - otherwise we
	 * could end up iterating over non-eligible zones endlessly.
	 */
	ac->preferred_zoneref = first_zones_zonelist(ac->zonelist,
					ac->highest_zoneidx, ac->nodemask);
	if (!zonelist_zone(ac->preferred_zoneref))
		goto nopage;

	/*
	 * Preserve ALLOC_NOFRAGMENT through the slowpath so that reclaim
	 * and compaction are tried before allowing clean superpageblocks
	 * to be tainted. The fast path sets this via alloc_flags_nofragment()
	 * but gfp_to_alloc_flags() only sets it for defrag_mode. Re-add it
	 * here so the slowpath retries with NOFRAGMENT still protecting
	 * clean SPBs until the last-resort drop below.
	 */
	alloc_flags |= alloc_flags_nofragment(
				zonelist_zone(ac->preferred_zoneref), gfp_mask);

	/*
	 * Check for insane configurations where the cpuset doesn't contain
	 * any suitable zone to satisfy the request - e.g. non-movable
	 * GFP_HIGHUSER allocations from MOVABLE nodes only.
	 */
	if (cpusets_insane_config() && (gfp_mask & __GFP_HARDWALL)) {
		struct zoneref *z = first_zones_zonelist(ac->zonelist,
					ac->highest_zoneidx,
					&cpuset_current_mems_allowed);
		if (!zonelist_zone(z))
			goto nopage;
	}

retry:
	/* Ensure kswapd doesn't accidentally go to sleep as long as we loop */
	if (alloc_flags & ALLOC_KSWAPD)
		wake_all_kswapds(order, gfp_mask, ac);

	/*
	 * The adjusted alloc_flags might result in immediate success, so try
	 * that first
	 */
	page = get_page_from_freelist(gfp_mask, order, alloc_flags, ac);
	if (page)
		goto got_pg;

	reserve_flags = __gfp_pfmemalloc_flags(gfp_mask);
	if (reserve_flags)
		alloc_flags = gfp_to_alloc_flags_cma(gfp_mask, reserve_flags) |
					  (alloc_flags & ALLOC_KSWAPD);

	/*
	 * Reset the nodemask and zonelist iterators if memory policies can be
	 * ignored. These allocations are high priority and system rather than
	 * user oriented.
	 */
	if (!(alloc_flags & ALLOC_CPUSET) || reserve_flags) {
		ac->nodemask = NULL;
		ac->preferred_zoneref = first_zones_zonelist(ac->zonelist,
					ac->highest_zoneidx, ac->nodemask);

		/*
		 * The first time we adjust anything due to being allowed to
		 * ignore memory policies or watermarks, retry immediately. This
		 * allows us to keep the first allocation attempt optimistic so
		 * it can succeed in a zone that is still above watermarks.
		 */
		if (can_retry_reserves) {
			can_retry_reserves = false;
			goto retry;
		}
	}

	/* Caller is not willing to reclaim, we can't balance anything */
	if (!can_direct_reclaim)
		goto nopage;

	/* Avoid recursion of direct reclaim */
	if (current->flags & PF_MEMALLOC)
		goto nopage;

	/* Try direct reclaim and then allocating */
	if (!compact_first) {
		page = __alloc_pages_direct_reclaim(gfp_mask, order, alloc_flags,
							ac, &did_some_progress);
		if (page)
			goto got_pg;
	}

	/* Try direct compaction and then allocating */
	page = __alloc_pages_direct_compact(gfp_mask, order, alloc_flags, ac,
					compact_priority, &compact_result);
	if (page)
		goto got_pg;

	if (compact_first) {
		/*
		 * THP page faults may attempt local node only first, but are
		 * then allowed to only compact, not reclaim, see
		 * alloc_pages_mpol().
		 *
		 * Compaction has failed above and we don't want such THP
		 * allocations to put reclaim pressure on a single node in a
		 * situation where other nodes might have plenty of available
		 * memory.
		 */
		if (gfp_has_flags(gfp_mask, __GFP_NORETRY | __GFP_THISNODE))
			goto nopage;

		/*
		 * For the initial compaction attempt we have lowered its
		 * priority. Restore it for further retries, if those are
		 * allowed. With __GFP_NORETRY there will be a single round of
		 * reclaim and compaction with the lowered priority.
		 */
		if (!(gfp_mask & __GFP_NORETRY))
			compact_priority = DEF_COMPACT_PRIORITY;

		compact_first = false;
		goto retry;
	}

	/* Do not loop if specifically requested */
	if (gfp_mask & __GFP_NORETRY)
		goto nopage;

	/*
	 * Do not retry costly high order allocations unless they are
	 * __GFP_RETRY_MAYFAIL and we can compact
	 */
	if (costly_order && (!can_compact ||
			     !(gfp_mask & __GFP_RETRY_MAYFAIL)))
		goto nopage;

	/*
	 * Deal with possible cpuset update races or zonelist updates to avoid
	 * infinite retries. No "goto retry;" can be placed above this check
	 * unless it can execute just once.
	 */
	if (check_retry_cpuset(cpuset_mems_cookie, ac) ||
	    check_retry_zonelist(zonelist_iter_cookie))
		goto restart;

	if (should_reclaim_retry(gfp_mask, order, ac, alloc_flags,
				 did_some_progress > 0, &no_progress_loops))
		goto retry;

	/*
	 * It doesn't make any sense to retry for the compaction if the order-0
	 * reclaim is not able to make any progress because the current
	 * implementation of the compaction depends on the sufficient amount
	 * of free memory (see __compaction_suitable)
	 */
	if (did_some_progress > 0 && can_compact &&
			should_compact_retry(ac, order, alloc_flags,
				compact_result, &compact_priority,
				&compaction_retries))
		goto retry;

	/*
	 * Reclaim and compaction have been tried but could not free enough
	 * pages in already-tainted superpageblocks. Before dropping
	 * NOFRAGMENT, try targeted evacuation of movable pages from
	 * tainted SPBs to create free pageblocks for unmovable claims.
	 */
	if ((alloc_flags & ALLOC_NOFRAGMENT) &&
	    (ac->migratetype == MIGRATE_UNMOVABLE ||
	     ac->migratetype == MIGRATE_RECLAIMABLE)) {
		struct zoneref *z;
		struct zone *zone;

		for_each_zone_zonelist_nodemask(zone, z, ac->zonelist,
					       ac->highest_zoneidx,
					       ac->nodemask) {
			if (spb_evacuate_for_order(zone, order,
						  ac->migratetype)) {
				page = get_page_from_freelist(gfp_mask, order,
							     alloc_flags, ac);
				if (page)
					goto got_pg;
			}
		}
	}

	/*
	 * Targeted evacuation could not free enough either. Drop
	 * NOFRAGMENT as a last resort to allow claiming from clean/empty
	 * SPBs. This is better than OOM-killing.
	 */
	if (alloc_flags & ALLOC_NOFRAGMENT) {
		alloc_flags &= ~ALLOC_NOFRAGMENT;
		goto retry;
	}

	/*
	 * Deal with possible cpuset update races or zonelist updates to avoid
	 * a unnecessary OOM kill.
	 */
	if (check_retry_cpuset(cpuset_mems_cookie, ac) ||
	    check_retry_zonelist(zonelist_iter_cookie))
		goto restart;

	/* Reclaim has failed us, start killing things */
	page = __alloc_pages_may_oom(gfp_mask, order, ac, &did_some_progress);
	if (page)
		goto got_pg;

	/* Avoid allocations with no watermarks from looping endlessly */
	if (tsk_is_oom_victim(current) &&
	    (alloc_flags & ALLOC_OOM ||
	     (gfp_mask & __GFP_NOMEMALLOC)))
		goto nopage;

	/* Retry as long as the OOM killer is making progress */
	if (did_some_progress) {
		no_progress_loops = 0;
		goto retry;
	}

nopage:
	/*
	 * Deal with possible cpuset update races or zonelist updates to avoid
	 * a unnecessary OOM kill.
	 */
	if (check_retry_cpuset(cpuset_mems_cookie, ac) ||
	    check_retry_zonelist(zonelist_iter_cookie))
		goto restart;

	/*
	 * Make sure that __GFP_NOFAIL request doesn't leak out and make sure
	 * we always retry
	 */
	if (unlikely(nofail)) {
		/*
		 * Lacking direct_reclaim we can't do anything to reclaim memory,
		 * we disregard these unreasonable nofail requests and still
		 * return NULL
		 */
		if (!can_direct_reclaim)
			goto fail;

		/*
		 * Help non-failing allocations by giving some access to memory
		 * reserves normally used for high priority non-blocking
		 * allocations but do not use ALLOC_NO_WATERMARKS because this
		 * could deplete whole memory reserves which would just make
		 * the situation worse.
		 */
		page = __alloc_pages_cpuset_fallback(gfp_mask, order, ALLOC_MIN_RESERVE, ac);
		if (page)
			goto got_pg;

		cond_resched();
		goto retry;
	}
fail:
	warn_alloc(gfp_mask, ac->nodemask,
			"page allocation failure: order:%u", order);
got_pg:
	return page;
}

static inline bool prepare_alloc_pages(gfp_t gfp_mask, unsigned int order,
		int preferred_nid, nodemask_t *nodemask,
		struct alloc_context *ac, gfp_t *alloc_gfp,
		unsigned int *alloc_flags)
{
	ac->highest_zoneidx = gfp_zone(gfp_mask);
	ac->zonelist = node_zonelist(preferred_nid, gfp_mask);
	ac->nodemask = nodemask;
	ac->migratetype = gfp_migratetype(gfp_mask);

	if (cpusets_enabled()) {
		*alloc_gfp |= __GFP_HARDWALL;
		/*
		 * When we are in the interrupt context, it is irrelevant
		 * to the current task context. It means that any node ok.
		 */
		if (in_task() && !ac->nodemask)
			ac->nodemask = &cpuset_current_mems_allowed;
		else
			*alloc_flags |= ALLOC_CPUSET;
	}

	might_alloc(gfp_mask);

	/*
	 * Don't invoke should_fail logic, since it may call
	 * get_random_u32() and printk() which need to spin_lock.
	 */
	if (!(*alloc_flags & ALLOC_TRYLOCK) &&
	    should_fail_alloc_page(gfp_mask, order))
		return false;

	*alloc_flags = gfp_to_alloc_flags_cma(gfp_mask, *alloc_flags);

	/* Dirty zone balancing only done in the fast path */
	ac->spread_dirty_pages = (gfp_mask & __GFP_WRITE);

	/*
	 * The preferred zone is used for statistics but crucially it is
	 * also used as the starting point for the zonelist iterator. It
	 * may get reset for allocations that ignore memory policies.
	 */
	ac->preferred_zoneref = first_zones_zonelist(ac->zonelist,
					ac->highest_zoneidx, ac->nodemask);

	return true;
}

/*
 * __alloc_pages_bulk - Allocate a number of order-0 pages to an array
 * @gfp: GFP flags for the allocation
 * @preferred_nid: The preferred NUMA node ID to allocate from
 * @nodemask: Set of nodes to allocate from, may be NULL
 * @nr_pages: The number of pages desired in the array
 * @page_array: Array to store the pages
 *
 * This is a batched version of the page allocator that attempts to allocate
 * @nr_pages quickly.  Pages are added to @page_array.
 *
 * Note that only the elements in @page_array that were cleared to %NULL on
 * entry are populated with newly allocated pages. @nr_pages is the maximum
 * number of pages that will be stored in the array.
 *
 * Returns the number of pages in @page_array, including ones already
 * allocated on entry.  This can be less than the number requested in @nr_pages,
 * but all empty slots are filled from the beginning.  I.e., if all slots in
 * @page_array were set to %NULL on entry, the slots from 0 to the return value
 * - 1 will be filled.
 */
unsigned long alloc_pages_bulk_noprof(gfp_t gfp, int preferred_nid,
			nodemask_t *nodemask, int nr_pages,
			struct page **page_array)
{
	struct page *page;
	unsigned long UP_flags;
	struct zone *zone;
	struct zoneref *z;
	struct per_cpu_pages *pcp;
	struct alloc_context ac;
	gfp_t alloc_gfp;
	unsigned int alloc_flags = ALLOC_WMARK_LOW;
	int nr_populated = 0, nr_account = 0;

	/*
	 * Skip populated array elements to determine if any pages need
	 * to be allocated before disabling IRQs.
	 */
	while (nr_populated < nr_pages && page_array[nr_populated])
		nr_populated++;

	/* No pages requested? */
	if (unlikely(nr_pages <= 0))
		goto out;

	/* Already populated array? */
	if (unlikely(nr_pages - nr_populated == 0))
		goto out;

	/* Bulk allocator does not support memcg accounting. */
	if (memcg_kmem_online() && (gfp & __GFP_ACCOUNT))
		goto failed;

	/* Use the single page allocator for one page. */
	if (nr_pages - nr_populated == 1)
		goto failed;

#ifdef CONFIG_PAGE_OWNER
	/*
	 * PAGE_OWNER may recurse into the allocator to allocate space to
	 * save the stack with pagesets.lock held. Releasing/reacquiring
	 * removes much of the performance benefit of bulk allocation so
	 * force the caller to allocate one page at a time as it'll have
	 * similar performance to added complexity to the bulk allocator.
	 */
	if (static_branch_unlikely(&page_owner_inited))
		goto failed;
#endif

	/* May set ALLOC_NOFRAGMENT, fragmentation will return 1 page. */
	gfp &= gfp_allowed_mask;
	alloc_gfp = gfp;
	if (!prepare_alloc_pages(gfp, 0, preferred_nid, nodemask, &ac, &alloc_gfp, &alloc_flags))
		goto out;
	gfp = alloc_gfp;

	alloc_flags |= alloc_flags_nofragment(zonelist_zone(ac.preferred_zoneref), gfp);

	/* Find an allowed local zone that meets the low watermark. */
	z = ac.preferred_zoneref;
	for_next_zone_zonelist_nodemask(zone, z, ac.highest_zoneidx, ac.nodemask) {
		unsigned long mark;

		if (cpusets_enabled() && (alloc_flags & ALLOC_CPUSET) &&
		    !__cpuset_zone_allowed(zone, gfp)) {
			continue;
		}

		if (nr_online_nodes > 1 && zone != zonelist_zone(ac.preferred_zoneref) &&
		    zone_to_nid(zone) != zonelist_node_idx(ac.preferred_zoneref)) {
			goto failed;
		}

		cond_accept_memory(zone, 0, alloc_flags);
retry_this_zone:
		mark = wmark_pages(zone, alloc_flags & ALLOC_WMARK_MASK) + nr_pages;
		if (zone_watermark_fast(zone, 0,  mark,
				zonelist_zone_idx(ac.preferred_zoneref),
				alloc_flags, gfp)) {
			break;
		}

		if (cond_accept_memory(zone, 0, alloc_flags))
			goto retry_this_zone;

		/* Try again if zone has deferred pages */
		if (deferred_pages_enabled()) {
			if (_deferred_grow_zone(zone, 0))
				goto retry_this_zone;
		}
	}

	/*
	 * If there are no allowed local zones that meets the watermarks then
	 * try to allocate a single page and reclaim if necessary.
	 */
	if (unlikely(!zone))
		goto failed;

	/* spin_trylock may fail due to a parallel drain or IRQ reentrancy. */
	pcp = pcp_spin_trylock(zone->per_cpu_pageset, UP_flags);
	if (!pcp)
		goto failed;

	/* Attempt the batch allocation */
	while (nr_populated < nr_pages) {

		/* Skip existing pages */
		if (page_array[nr_populated]) {
			nr_populated++;
			continue;
		}

		page = __rmqueue_pcplist(zone, 0, ac.migratetype, alloc_flags, pcp);
		if (unlikely(!page)) {
			/* Try and allocate at least one page */
			if (!nr_account) {
				pcp_spin_unlock(pcp, UP_flags);
				goto failed;
			}
			break;
		}
		nr_account++;

		prep_new_page(page, 0, gfp, 0);
		set_page_refcounted(page);
		page_array[nr_populated++] = page;
	}

	pcp_spin_unlock(pcp, UP_flags);

	__count_zid_vm_events(PGALLOC, zone_idx(zone), nr_account);
	zone_statistics(zonelist_zone(ac.preferred_zoneref), zone, nr_account);

out:
	return nr_populated;

failed:
	page = __alloc_pages_noprof(gfp, 0, preferred_nid, nodemask);
	if (page)
		page_array[nr_populated++] = page;
	goto out;
}
EXPORT_SYMBOL_GPL(alloc_pages_bulk_noprof);

/*
 * This is the 'heart' of the zoned buddy allocator.
 */
struct page *__alloc_frozen_pages_noprof(gfp_t gfp, unsigned int order,
		int preferred_nid, nodemask_t *nodemask)
{
	struct page *page;
	unsigned int alloc_flags = ALLOC_WMARK_LOW;
	gfp_t alloc_gfp; /* The gfp_t that was actually used for allocation */
	struct alloc_context ac = { };

	/*
	 * There are several places where we assume that the order value is sane
	 * so bail out early if the request is out of bound.
	 */
	if (WARN_ON_ONCE_GFP(order > MAX_PAGE_ORDER, gfp))
		return NULL;

	gfp &= gfp_allowed_mask;
	/*
	 * Apply scoped allocation constraints. This is mainly about GFP_NOFS
	 * resp. GFP_NOIO which has to be inherited for all allocation requests
	 * from a particular context which has been marked by
	 * memalloc_no{fs,io}_{save,restore}. And PF_MEMALLOC_PIN which ensures
	 * movable zones are not used during allocation.
	 */
	gfp = current_gfp_context(gfp);
	alloc_gfp = gfp;
	if (!prepare_alloc_pages(gfp, order, preferred_nid, nodemask, &ac,
			&alloc_gfp, &alloc_flags))
		return NULL;

	/*
	 * Forbid the first pass from falling back to types that fragment
	 * memory until all local zones are considered.
	 */
	alloc_flags |= alloc_flags_nofragment(zonelist_zone(ac.preferred_zoneref), gfp);

	/* First allocation attempt */
	page = get_page_from_freelist(alloc_gfp, order, alloc_flags, &ac);
	if (likely(page))
		goto out;

	alloc_gfp = gfp;
	ac.spread_dirty_pages = false;

	/*
	 * Restore the original nodemask if it was potentially replaced with
	 * &cpuset_current_mems_allowed to optimize the fast-path attempt.
	 */
	ac.nodemask = nodemask;

	page = __alloc_pages_slowpath(alloc_gfp, order, &ac);

out:
	if (memcg_kmem_online() && (gfp & __GFP_ACCOUNT) && page &&
	    unlikely(__memcg_kmem_charge_page(page, gfp, order) != 0)) {
		free_frozen_pages(page, order);
		page = NULL;
	}

	trace_mm_page_alloc(page, order, alloc_gfp, ac.migratetype);
	kmsan_alloc_page(page, order, alloc_gfp);

	return page;
}
EXPORT_SYMBOL(__alloc_frozen_pages_noprof);

struct page *__alloc_pages_noprof(gfp_t gfp, unsigned int order,
		int preferred_nid, nodemask_t *nodemask)
{
	struct page *page;

	page = __alloc_frozen_pages_noprof(gfp, order, preferred_nid, nodemask);
	if (page)
		set_page_refcounted(page);
	return page;
}
EXPORT_SYMBOL(__alloc_pages_noprof);

struct folio *__folio_alloc_noprof(gfp_t gfp, unsigned int order, int preferred_nid,
		nodemask_t *nodemask)
{
	struct page *page = __alloc_pages_noprof(gfp | __GFP_COMP, order,
					preferred_nid, nodemask);
	return page_rmappable_folio(page);
}
EXPORT_SYMBOL(__folio_alloc_noprof);

/*
 * Common helper functions. Never use with __GFP_HIGHMEM because the returned
 * address cannot represent highmem pages. Use alloc_pages and then kmap if
 * you need to access high mem.
 */
unsigned long get_free_pages_noprof(gfp_t gfp_mask, unsigned int order)
{
	struct page *page;

	page = alloc_pages_noprof(gfp_mask & ~__GFP_HIGHMEM, order);
	if (!page)
		return 0;
	return (unsigned long) page_address(page);
}
EXPORT_SYMBOL(get_free_pages_noprof);

unsigned long get_zeroed_page_noprof(gfp_t gfp_mask)
{
	return get_free_pages_noprof(gfp_mask | __GFP_ZERO, 0);
}
EXPORT_SYMBOL(get_zeroed_page_noprof);

static void ___free_pages(struct page *page, unsigned int order,
			  fpi_t fpi_flags)
{
	/* get PageHead before we drop reference */
	int head = PageHead(page);
	/* get alloc tag in case the page is released by others */
	struct alloc_tag *tag = pgalloc_tag_get(page);

	if (put_page_testzero(page))
		__free_frozen_pages(page, order, fpi_flags);
	else if (!head) {
		pgalloc_tag_sub_pages(tag, (1 << order) - 1);
		while (order-- > 0) {
			/*
			 * The "tail" pages of this non-compound high-order
			 * page will have no code tags, so to avoid warnings
			 * mark them as empty.
			 */
			clear_page_tag_ref(page + (1 << order));
			__free_frozen_pages(page + (1 << order), order,
					    fpi_flags);
		}
	}
}

/**
 * __free_pages - Free pages allocated with alloc_pages().
 * @page: The page pointer returned from alloc_pages().
 * @order: The order of the allocation.
 *
 * This function can free multi-page allocations that are not compound
 * pages.  It does not check that the @order passed in matches that of
 * the allocation, so it is easy to leak memory.  Freeing more memory
 * than was allocated will probably emit a warning.
 *
 * If the last reference to this page is speculative, it will be released
 * by put_page() which only frees the first page of a non-compound
 * allocation.  To prevent the remaining pages from being leaked, we free
 * the subsequent pages here.  If you want to use the page's reference
 * count to decide when to free the allocation, you should allocate a
 * compound page, and use put_page() instead of __free_pages().
 *
 * Context: May be called in interrupt context or while holding a normal
 * spinlock, but not in NMI context or while holding a raw spinlock.
 */
void __free_pages(struct page *page, unsigned int order)
{
	___free_pages(page, order, FPI_NONE);
}
EXPORT_SYMBOL(__free_pages);

/*
 * Can be called while holding raw_spin_lock or from IRQ and NMI for any
 * page type (not only those that came from alloc_pages_nolock)
 */
void free_pages_nolock(struct page *page, unsigned int order)
{
	___free_pages(page, order, FPI_TRYLOCK);
}

/**
 * free_pages - Free pages allocated with __get_free_pages().
 * @addr: The virtual address tied to a page returned from __get_free_pages().
 * @order: The order of the allocation.
 *
 * This function behaves the same as __free_pages(). Use this function
 * to free pages when you only have a valid virtual address. If you have
 * the page, call __free_pages() instead.
 */
void free_pages(unsigned long addr, unsigned int order)
{
	if (addr != 0) {
		VM_BUG_ON(!virt_addr_valid((void *)addr));
		__free_pages(virt_to_page((void *)addr), order);
	}
}

EXPORT_SYMBOL(free_pages);

static void *make_alloc_exact(unsigned long addr, unsigned int order,
		size_t size)
{
	if (addr) {
		unsigned long nr = DIV_ROUND_UP(size, PAGE_SIZE);
		struct page *page = virt_to_page((void *)addr);
		struct page *last = page + nr;

		__split_page(page, order);
		while (page < --last)
			set_page_refcounted(last);

		last = page + (1UL << order);
		for (page += nr; page < last; page++)
			__free_pages_ok(page, 0, FPI_TO_TAIL);
	}
	return (void *)addr;
}

/**
 * alloc_pages_exact - allocate an exact number physically-contiguous pages.
 * @size: the number of bytes to allocate
 * @gfp_mask: GFP flags for the allocation, must not contain __GFP_COMP
 *
 * This function is similar to alloc_pages(), except that it allocates the
 * minimum number of pages to satisfy the request.  alloc_pages() can only
 * allocate memory in power-of-two pages.
 *
 * This function is also limited by MAX_PAGE_ORDER.
 *
 * Memory allocated by this function must be released by free_pages_exact().
 *
 * Return: pointer to the allocated area or %NULL in case of error.
 */
void *alloc_pages_exact_noprof(size_t size, gfp_t gfp_mask)
{
	unsigned int order = get_order(size);
	unsigned long addr;

	if (WARN_ON_ONCE(gfp_mask & (__GFP_COMP | __GFP_HIGHMEM)))
		gfp_mask &= ~(__GFP_COMP | __GFP_HIGHMEM);

	addr = get_free_pages_noprof(gfp_mask, order);
	return make_alloc_exact(addr, order, size);
}
EXPORT_SYMBOL(alloc_pages_exact_noprof);

/**
 * alloc_pages_exact_nid - allocate an exact number of physically-contiguous
 *			   pages on a node.
 * @nid: the preferred node ID where memory should be allocated
 * @size: the number of bytes to allocate
 * @gfp_mask: GFP flags for the allocation, must not contain __GFP_COMP
 *
 * Like alloc_pages_exact(), but try to allocate on node nid first before falling
 * back.
 *
 * Return: pointer to the allocated area or %NULL in case of error.
 */
void * __meminit alloc_pages_exact_nid_noprof(int nid, size_t size, gfp_t gfp_mask)
{
	unsigned int order = get_order(size);
	struct page *p;

	if (WARN_ON_ONCE(gfp_mask & (__GFP_COMP | __GFP_HIGHMEM)))
		gfp_mask &= ~(__GFP_COMP | __GFP_HIGHMEM);

	p = alloc_pages_node_noprof(nid, gfp_mask, order);
	if (!p)
		return NULL;
	return make_alloc_exact((unsigned long)page_address(p), order, size);
}

/**
 * free_pages_exact - release memory allocated via alloc_pages_exact()
 * @virt: the value returned by alloc_pages_exact.
 * @size: size of allocation, same value as passed to alloc_pages_exact().
 *
 * Release the memory allocated by a previous call to alloc_pages_exact.
 */
void free_pages_exact(void *virt, size_t size)
{
	unsigned long addr = (unsigned long)virt;
	unsigned long end = addr + PAGE_ALIGN(size);

	while (addr < end) {
		free_page(addr);
		addr += PAGE_SIZE;
	}
}
EXPORT_SYMBOL(free_pages_exact);

/**
 * nr_free_zone_pages - count number of pages beyond high watermark
 * @offset: The zone index of the highest zone
 *
 * nr_free_zone_pages() counts the number of pages which are beyond the
 * high watermark within all zones at or below a given zone index.  For each
 * zone, the number of pages is calculated as:
 *
 *     nr_free_zone_pages = managed_pages - high_pages
 *
 * Return: number of pages beyond high watermark.
 */
static unsigned long nr_free_zone_pages(int offset)
{
	struct zoneref *z;
	struct zone *zone;

	/* Just pick one node, since fallback list is circular */
	unsigned long sum = 0;

	struct zonelist *zonelist = node_zonelist(numa_node_id(), GFP_KERNEL);

	for_each_zone_zonelist(zone, z, zonelist, offset) {
		unsigned long size = zone_managed_pages(zone);
		unsigned long high = high_wmark_pages(zone);
		if (size > high)
			sum += size - high;
	}

	return sum;
}

/**
 * nr_free_buffer_pages - count number of pages beyond high watermark
 *
 * nr_free_buffer_pages() counts the number of pages which are beyond the high
 * watermark within ZONE_DMA and ZONE_NORMAL.
 *
 * Return: number of pages beyond high watermark within ZONE_DMA and
 * ZONE_NORMAL.
 */
unsigned long nr_free_buffer_pages(void)
{
	return nr_free_zone_pages(gfp_zone(GFP_USER));
}
EXPORT_SYMBOL_GPL(nr_free_buffer_pages);

static void zoneref_set_zone(struct zone *zone, struct zoneref *zoneref)
{
	zoneref->zone = zone;
	zoneref->zone_idx = zone_idx(zone);
}

/*
 * Builds allocation fallback zone lists.
 *
 * Add all populated zones of a node to the zonelist.
 */
static int build_zonerefs_node(pg_data_t *pgdat, struct zoneref *zonerefs)
{
	struct zone *zone;
	enum zone_type zone_type = MAX_NR_ZONES;
	int nr_zones = 0;

	do {
		zone_type--;
		zone = pgdat->node_zones + zone_type;
		if (populated_zone(zone)) {
			zoneref_set_zone(zone, &zonerefs[nr_zones++]);
			check_highest_zone(zone_type);
		}
	} while (zone_type);

	return nr_zones;
}

#ifdef CONFIG_NUMA

static int __parse_numa_zonelist_order(char *s)
{
	/*
	 * We used to support different zonelists modes but they turned
	 * out to be just not useful. Let's keep the warning in place
	 * if somebody still use the cmd line parameter so that we do
	 * not fail it silently
	 */
	if (!(*s == 'd' || *s == 'D' || *s == 'n' || *s == 'N')) {
		pr_warn("Ignoring unsupported numa_zonelist_order value:  %s\n", s);
		return -EINVAL;
	}
	return 0;
}

static char numa_zonelist_order[] = "Node";
#define NUMA_ZONELIST_ORDER_LEN	16
/*
 * sysctl handler for numa_zonelist_order
 */
static int numa_zonelist_order_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	if (write)
		return __parse_numa_zonelist_order(buffer);
	return proc_dostring(table, write, buffer, length, ppos);
}

static int node_load[MAX_NUMNODES];

/**
 * find_next_best_node - find the next node that should appear in a given node's fallback list
 * @node: node whose fallback list we're appending
 * @used_node_mask: nodemask_t of already used nodes
 *
 * We use a number of factors to determine which is the next node that should
 * appear on a given node's fallback list.  The node should not have appeared
 * already in @node's fallback list, and it should be the next closest node
 * according to the distance array (which contains arbitrary distance values
 * from each node to each node in the system), and should also prefer nodes
 * with no CPUs, since presumably they'll have very little allocation pressure
 * on them otherwise.
 *
 * Return: node id of the found node or %NUMA_NO_NODE if no node is found.
 */
int find_next_best_node(int node, nodemask_t *used_node_mask)
{
	int n, val;
	int min_val = INT_MAX;
	int best_node = NUMA_NO_NODE;

	/*
	 * Use the local node if we haven't already, but for memoryless local
	 * node, we should skip it and fall back to other nodes.
	 */
	if (!node_isset(node, *used_node_mask) && node_state(node, N_MEMORY)) {
		node_set(node, *used_node_mask);
		return node;
	}

	for_each_node_state(n, N_MEMORY) {

		/* Don't want a node to appear more than once */
		if (node_isset(n, *used_node_mask))
			continue;

		/* Use the distance array to find the distance */
		val = node_distance(node, n);

		/* Penalize nodes under us ("prefer the next node") */
		val += (n < node);

		/* Give preference to headless and unused nodes */
		if (!cpumask_empty(cpumask_of_node(n)))
			val += PENALTY_FOR_NODE_WITH_CPUS;

		/* Slight preference for less loaded node */
		val *= MAX_NUMNODES;
		val += node_load[n];

		if (val < min_val) {
			min_val = val;
			best_node = n;
		}
	}

	if (best_node >= 0)
		node_set(best_node, *used_node_mask);

	return best_node;
}


/*
 * Build zonelists ordered by node and zones within node.
 * This results in maximum locality--normal zone overflows into local
 * DMA zone, if any--but risks exhausting DMA zone.
 */
static void build_zonelists_in_node_order(pg_data_t *pgdat, int *node_order,
		unsigned nr_nodes)
{
	struct zoneref *zonerefs;
	int i;

	zonerefs = pgdat->node_zonelists[ZONELIST_FALLBACK]._zonerefs;

	for (i = 0; i < nr_nodes; i++) {
		int nr_zones;

		pg_data_t *node = NODE_DATA(node_order[i]);

		nr_zones = build_zonerefs_node(node, zonerefs);
		zonerefs += nr_zones;
	}
	zonerefs->zone = NULL;
	zonerefs->zone_idx = 0;
}

/*
 * Build __GFP_THISNODE zonelists
 */
static void build_thisnode_zonelists(pg_data_t *pgdat)
{
	struct zoneref *zonerefs;
	int nr_zones;

	zonerefs = pgdat->node_zonelists[ZONELIST_NOFALLBACK]._zonerefs;
	nr_zones = build_zonerefs_node(pgdat, zonerefs);
	zonerefs += nr_zones;
	zonerefs->zone = NULL;
	zonerefs->zone_idx = 0;
}

static void build_zonelists(pg_data_t *pgdat)
{
	static int node_order[MAX_NUMNODES];
	int node, nr_nodes = 0;
	nodemask_t used_mask = NODE_MASK_NONE;
	int local_node, prev_node;

	/* NUMA-aware ordering of nodes */
	local_node = pgdat->node_id;
	prev_node = local_node;

	memset(node_order, 0, sizeof(node_order));
	while ((node = find_next_best_node(local_node, &used_mask)) >= 0) {
		/*
		 * We don't want to pressure a particular node.
		 * So adding penalty to the first node in same
		 * distance group to make it round-robin.
		 */
		if (node_distance(local_node, node) !=
		    node_distance(local_node, prev_node))
			node_load[node] += 1;

		node_order[nr_nodes++] = node;
		prev_node = node;
	}

	build_zonelists_in_node_order(pgdat, node_order, nr_nodes);
	build_thisnode_zonelists(pgdat);
	pr_info("Fallback order for Node %d: ", local_node);
	for (node = 0; node < nr_nodes; node++)
		pr_cont("%d ", node_order[node]);
	pr_cont("\n");
}

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * Return node id of node used for "local" allocations.
 * I.e., first node id of first zone in arg node's generic zonelist.
 * Used for initializing percpu 'numa_mem', which is used primarily
 * for kernel allocations, so use GFP_KERNEL flags to locate zonelist.
 */
int local_memory_node(int node)
{
	struct zoneref *z;

	z = first_zones_zonelist(node_zonelist(node, GFP_KERNEL),
				   gfp_zone(GFP_KERNEL),
				   NULL);
	return zonelist_node_idx(z);
}
#endif

static void setup_min_unmapped_ratio(void);
static void setup_min_slab_ratio(void);
#else	/* CONFIG_NUMA */

static void build_zonelists(pg_data_t *pgdat)
{
	struct zoneref *zonerefs;
	int nr_zones;

	zonerefs = pgdat->node_zonelists[ZONELIST_FALLBACK]._zonerefs;
	nr_zones = build_zonerefs_node(pgdat, zonerefs);
	zonerefs += nr_zones;

	zonerefs->zone = NULL;
	zonerefs->zone_idx = 0;
}

#endif	/* CONFIG_NUMA */

/*
 * Boot pageset table. One per cpu which is going to be used for all
 * zones and all nodes. The parameters will be set in such a way
 * that an item put on a list will immediately be handed over to
 * the buddy list. This is safe since pageset manipulation is done
 * with interrupts disabled.
 *
 * The boot_pagesets must be kept even after bootup is complete for
 * unused processors and/or zones. They do play a role for bootstrapping
 * hotplugged processors.
 *
 * zoneinfo_show() and maybe other functions do
 * not check if the processor is online before following the pageset pointer.
 * Other parts of the kernel may not check if the zone is available.
 */
static void per_cpu_pages_init(struct per_cpu_pages *pcp, struct per_cpu_zonestat *pzstats);
/* These effectively disable the pcplists in the boot pageset completely */
#define BOOT_PAGESET_HIGH	0
#define BOOT_PAGESET_BATCH	1
static DEFINE_PER_CPU(struct per_cpu_pages, boot_pageset);
static DEFINE_PER_CPU(struct per_cpu_zonestat, boot_zonestats);

static void __build_all_zonelists(void *data)
{
	int nid;
	int __maybe_unused cpu;
	pg_data_t *self = data;
	unsigned long flags;

	/*
	 * The zonelist_update_seq must be acquired with irqsave because the
	 * reader can be invoked from IRQ with GFP_ATOMIC.
	 */
	write_seqlock_irqsave(&zonelist_update_seq, flags);
	/*
	 * Also disable synchronous printk() to prevent any printk() from
	 * trying to hold port->lock, for
	 * tty_insert_flip_string_and_push_buffer() on other CPU might be
	 * calling kmalloc(GFP_ATOMIC | __GFP_NOWARN) with port->lock held.
	 */
	printk_deferred_enter();

#ifdef CONFIG_NUMA
	memset(node_load, 0, sizeof(node_load));
#endif

	/*
	 * This node is hotadded and no memory is yet present.   So just
	 * building zonelists is fine - no need to touch other nodes.
	 */
	if (self && !node_online(self->node_id)) {
		build_zonelists(self);
	} else {
		/*
		 * All possible nodes have pgdat preallocated
		 * in free_area_init
		 */
		for_each_node(nid) {
			pg_data_t *pgdat = NODE_DATA(nid);

			build_zonelists(pgdat);
		}

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
		/*
		 * We now know the "local memory node" for each node--
		 * i.e., the node of the first zone in the generic zonelist.
		 * Set up numa_mem percpu variable for on-line cpus.  During
		 * boot, only the boot cpu should be on-line;  we'll init the
		 * secondary cpus' numa_mem as they come on-line.  During
		 * node/memory hotplug, we'll fixup all on-line cpus.
		 */
		for_each_online_cpu(cpu)
			set_cpu_numa_mem(cpu, local_memory_node(cpu_to_node(cpu)));
#endif
	}

	printk_deferred_exit();
	write_sequnlock_irqrestore(&zonelist_update_seq, flags);
}

static noinline void __init
build_all_zonelists_init(void)
{
	int cpu;

	__build_all_zonelists(NULL);

	/*
	 * Initialize the boot_pagesets that are going to be used
	 * for bootstrapping processors. The real pagesets for
	 * each zone will be allocated later when the per cpu
	 * allocator is available.
	 *
	 * boot_pagesets are used also for bootstrapping offline
	 * cpus if the system is already booted because the pagesets
	 * are needed to initialize allocators on a specific cpu too.
	 * F.e. the percpu allocator needs the page allocator which
	 * needs the percpu allocator in order to allocate its pagesets
	 * (a chicken-egg dilemma).
	 */
	for_each_possible_cpu(cpu)
		per_cpu_pages_init(&per_cpu(boot_pageset, cpu), &per_cpu(boot_zonestats, cpu));

	mminit_verify_zonelist();
	cpuset_init_current_mems_allowed();
}

/*
 * unless system_state == SYSTEM_BOOTING.
 *
 * __ref due to call of __init annotated helper build_all_zonelists_init
 * [protected by SYSTEM_BOOTING].
 */
void __ref build_all_zonelists(pg_data_t *pgdat)
{
	unsigned long vm_total_pages;

	if (system_state == SYSTEM_BOOTING) {
		build_all_zonelists_init();
	} else {
		__build_all_zonelists(pgdat);
		/* cpuset refresh routine should be here */
	}
	/* Get the number of free pages beyond high watermark in all zones. */
	vm_total_pages = nr_free_zone_pages(gfp_zone(GFP_HIGHUSER_MOVABLE));
	/*
	 * Disable grouping by mobility if the number of pages in the
	 * system is too low to allow the mechanism to work. It would be
	 * more accurate, but expensive to check per-zone. This check is
	 * made on memory-hotadd so a system can start with mobility
	 * disabled and enable it later
	 */
	if (vm_total_pages < (pageblock_nr_pages * MIGRATE_TYPES))
		page_group_by_mobility_disabled = 1;
	else
		page_group_by_mobility_disabled = 0;

	pr_info("Built %u zonelists, mobility grouping %s.  Total pages: %ld\n",
		nr_online_nodes,
		str_off_on(page_group_by_mobility_disabled),
		vm_total_pages);
#ifdef CONFIG_NUMA
	pr_info("Policy zone: %s\n", zone_names[policy_zone]);
#endif
}

static int zone_batchsize(struct zone *zone)
{
#ifdef CONFIG_MMU
	int batch;

	/*
	 * The number of pages to batch allocate is either ~0.025%
	 * of the zone or 256KB, whichever is smaller. The batch
	 * size is striking a balance between allocation latency
	 * and zone lock contention.
	 */
	batch = min(zone_managed_pages(zone) >> 12, SZ_256K / PAGE_SIZE);
	if (batch <= 1)
		return 1;

	/*
	 * Clamp the batch to a 2^n - 1 value. Having a power
	 * of 2 value was found to be more likely to have
	 * suboptimal cache aliasing properties in some cases.
	 *
	 * For example if 2 tasks are alternately allocating
	 * batches of pages, one task can end up with a lot
	 * of pages of one half of the possible page colors
	 * and the other with pages of the other colors.
	 */
	batch = rounddown_pow_of_two(batch + batch/2) - 1;

	return batch;

#else
	/* The deferral and batching of frees should be suppressed under NOMMU
	 * conditions.
	 *
	 * The problem is that NOMMU needs to be able to allocate large chunks
	 * of contiguous memory as there's no hardware page translation to
	 * assemble apparent contiguous memory from discontiguous pages.
	 *
	 * Queueing large contiguous runs of pages for batching, however,
	 * causes the pages to actually be freed in smaller chunks.  As there
	 * can be a significant delay between the individual batches being
	 * recycled, this leads to the once large chunks of space being
	 * fragmented and becoming unavailable for high-order allocations.
	 */
	return 1;
#endif
}

static int percpu_pagelist_high_fraction;
static int zone_highsize(struct zone *zone, int batch, int cpu_online,
			 int high_fraction)
{
#ifdef CONFIG_MMU
	int high;
	int nr_split_cpus;
	unsigned long total_pages;

	if (!high_fraction) {
		/*
		 * By default, the high value of the pcp is based on the zone
		 * low watermark so that if they are full then background
		 * reclaim will not be started prematurely.
		 */
		total_pages = low_wmark_pages(zone);
	} else {
		/*
		 * If percpu_pagelist_high_fraction is configured, the high
		 * value is based on a fraction of the managed pages in the
		 * zone.
		 */
		total_pages = zone_managed_pages(zone) / high_fraction;
	}

	/*
	 * Split the high value across all online CPUs local to the zone. Note
	 * that early in boot that CPUs may not be online yet and that during
	 * CPU hotplug that the cpumask is not yet updated when a CPU is being
	 * onlined. For memory nodes that have no CPUs, split the high value
	 * across all online CPUs to mitigate the risk that reclaim is triggered
	 * prematurely due to pages stored on pcp lists.
	 */
	nr_split_cpus = cpumask_weight(cpumask_of_node(zone_to_nid(zone))) + cpu_online;
	if (!nr_split_cpus)
		nr_split_cpus = num_online_cpus();
	high = total_pages / nr_split_cpus;

	/*
	 * Ensure high is at least batch*4. The multiple is based on the
	 * historical relationship between high and batch.
	 */
	high = max(high, batch << 2);

	return high;
#else
	return 0;
#endif
}

/*
 * pcp->high and pcp->batch values are related and generally batch is lower
 * than high. They are also related to pcp->count such that count is lower
 * than high, and as soon as it reaches high, the pcplist is flushed.
 *
 * However, guaranteeing these relations at all times would require e.g. write
 * barriers here but also careful usage of read barriers at the read side, and
 * thus be prone to error and bad for performance. Thus the update only prevents
 * store tearing. Any new users of pcp->batch, pcp->high_min and pcp->high_max
 * should ensure they can cope with those fields changing asynchronously, and
 * fully trust only the pcp->count field on the local CPU with interrupts
 * disabled.
 *
 * mutex_is_locked(&pcp_batch_high_lock) required when calling this function
 * outside of boot time (or some other assurance that no concurrent updaters
 * exist).
 */
static void pageset_update(struct per_cpu_pages *pcp, unsigned long high_min,
			   unsigned long high_max, unsigned long batch)
{
	WRITE_ONCE(pcp->batch, batch);
	WRITE_ONCE(pcp->high_min, high_min);
	WRITE_ONCE(pcp->high_max, high_max);
}

static void per_cpu_pages_init(struct per_cpu_pages *pcp, struct per_cpu_zonestat *pzstats)
{
	int pindex;

	memset(pcp, 0, sizeof(*pcp));
	memset(pzstats, 0, sizeof(*pzstats));

	spin_lock_init(&pcp->lock);
	for (pindex = 0; pindex < NR_PCP_LISTS; pindex++)
		INIT_LIST_HEAD(&pcp->lists[pindex]);
	INIT_LIST_HEAD(&pcp->owned_blocks);

	/*
	 * Set batch and high values safe for a boot pageset. A true percpu
	 * pageset's initialization will update them subsequently. Here we don't
	 * need to be as careful as pageset_update() as nobody can access the
	 * pageset yet.
	 */
	pcp->high_min = BOOT_PAGESET_HIGH;
	pcp->high_max = BOOT_PAGESET_HIGH;
	pcp->batch = BOOT_PAGESET_BATCH;
}

static void __zone_set_pageset_high_and_batch(struct zone *zone, unsigned long high_min,
					      unsigned long high_max, unsigned long batch)
{
	struct per_cpu_pages *pcp;
	int cpu;

	for_each_possible_cpu(cpu) {
		pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
		pageset_update(pcp, high_min, high_max, batch);
	}
}

/*
 * Calculate and set new high and batch values for all per-cpu pagesets of a
 * zone based on the zone's size.
 */
static void zone_set_pageset_high_and_batch(struct zone *zone, int cpu_online)
{
	int new_high_min, new_high_max, new_batch;

	new_batch = zone_batchsize(zone);
	if (percpu_pagelist_high_fraction) {
		new_high_min = zone_highsize(zone, new_batch, cpu_online,
					     percpu_pagelist_high_fraction);
		/*
		 * PCP high is tuned manually, disable auto-tuning via
		 * setting high_min and high_max to the manual value.
		 */
		new_high_max = new_high_min;
	} else {
		new_high_min = zone_highsize(zone, new_batch, cpu_online, 0);
		new_high_max = zone_highsize(zone, new_batch, cpu_online,
					     MIN_PERCPU_PAGELIST_HIGH_FRACTION);
	}

	if (zone->pageset_high_min == new_high_min &&
	    zone->pageset_high_max == new_high_max &&
	    zone->pageset_batch == new_batch)
		return;

	zone->pageset_high_min = new_high_min;
	zone->pageset_high_max = new_high_max;
	zone->pageset_batch = new_batch;

	__zone_set_pageset_high_and_batch(zone, new_high_min, new_high_max,
					  new_batch);
}

void __meminit setup_zone_pageset(struct zone *zone)
{
	int cpu;

	/* Size may be 0 on !SMP && !NUMA */
	if (sizeof(struct per_cpu_zonestat) > 0)
		zone->per_cpu_zonestats = alloc_percpu(struct per_cpu_zonestat);

	zone->per_cpu_pageset = alloc_percpu(struct per_cpu_pages);
	for_each_possible_cpu(cpu) {
		struct per_cpu_pages *pcp;
		struct per_cpu_zonestat *pzstats;

		pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
		pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
		per_cpu_pages_init(pcp, pzstats);
	}

	zone_set_pageset_high_and_batch(zone, 0);
}

/*
 * The zone indicated has a new number of managed_pages; batch sizes and percpu
 * page high values need to be recalculated.
 */
static void zone_pcp_update(struct zone *zone, int cpu_online)
{
	mutex_lock(&pcp_batch_high_lock);
	zone_set_pageset_high_and_batch(zone, cpu_online);
	mutex_unlock(&pcp_batch_high_lock);
}

static void zone_pcp_update_cacheinfo(struct zone *zone, unsigned int cpu)
{
	struct per_cpu_pages *pcp;
	struct cpu_cacheinfo *cci;
	unsigned long UP_flags;

	pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
	cci = get_cpu_cacheinfo(cpu);
	/*
	 * If data cache slice of CPU is large enough, "pcp->batch"
	 * pages can be preserved in PCP before draining PCP for
	 * consecutive high-order pages freeing without allocation.
	 * This can reduce zone lock contention without hurting
	 * cache-hot pages sharing.
	 */
	pcp_spin_lock_maybe_irqsave(pcp, UP_flags);
	if ((cci->per_cpu_data_slice_size >> PAGE_SHIFT) > 3 * pcp->batch)
		pcp->flags |= PCPF_FREE_HIGH_BATCH;
	else
		pcp->flags &= ~PCPF_FREE_HIGH_BATCH;
	pcp_spin_unlock_maybe_irqrestore(pcp, UP_flags);
}

void setup_pcp_cacheinfo(unsigned int cpu)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		zone_pcp_update_cacheinfo(zone, cpu);
}

/*
 * Allocate per cpu pagesets and initialize them.
 * Before this call only boot pagesets were available.
 */
void __init setup_per_cpu_pageset(void)
{
	struct pglist_data *pgdat;
	struct zone *zone;
	int __maybe_unused cpu;

	for_each_populated_zone(zone)
		setup_zone_pageset(zone);

#ifdef CONFIG_NUMA
	/*
	 * Unpopulated zones continue using the boot pagesets.
	 * The numa stats for these pagesets need to be reset.
	 * Otherwise, they will end up skewing the stats of
	 * the nodes these zones are associated with.
	 */
	for_each_possible_cpu(cpu) {
		struct per_cpu_zonestat *pzstats = &per_cpu(boot_zonestats, cpu);
		memset(pzstats->vm_numa_event, 0,
		       sizeof(pzstats->vm_numa_event));
	}
#endif

	for_each_online_pgdat(pgdat)
		pgdat->per_cpu_nodestats =
			alloc_percpu(struct per_cpu_nodestat);
}

__meminit void zone_pcp_init(struct zone *zone)
{
	/*
	 * per cpu subsystem is not up at this point. The following code
	 * relies on the ability of the linker to provide the
	 * offset of a (static) per cpu variable into the per cpu area.
	 */
	zone->per_cpu_pageset = &boot_pageset;
	zone->per_cpu_zonestats = &boot_zonestats;
	zone->pageset_high_min = BOOT_PAGESET_HIGH;
	zone->pageset_high_max = BOOT_PAGESET_HIGH;
	zone->pageset_batch = BOOT_PAGESET_BATCH;

	if (populated_zone(zone))
		pr_debug("  %s zone: %lu pages, LIFO batch:%u\n", zone->name,
			 zone->present_pages, zone_batchsize(zone));
}

static void setup_per_zone_lowmem_reserve(void);

void adjust_managed_page_count(struct page *page, long count)
{
	atomic_long_add(count, &page_zone(page)->managed_pages);
	totalram_pages_add(count);
	setup_per_zone_lowmem_reserve();
}
EXPORT_SYMBOL(adjust_managed_page_count);

void free_reserved_page(struct page *page)
{
	clear_page_tag_ref(page);
	ClearPageReserved(page);
	init_page_count(page);
	__free_page(page);
	adjust_managed_page_count(page, 1);
}
EXPORT_SYMBOL(free_reserved_page);

static int page_alloc_cpu_dead(unsigned int cpu)
{
	struct zone *zone;

	lru_add_drain_cpu(cpu);
	mlock_drain_remote(cpu);

	/*
	 * Mark the dead CPU's PCPs so concurrent frees don't
	 * enqueue pages on them after the drain. Set the flag
	 * under pcp->lock to serialize with trylock in the free
	 * path. Stale ownership entries in pageblock_data are
	 * harmless: frees check PCPF_CPU_DEAD and fall back to zone,
	 * and rmqueue_bulk will reclaim the blocks for live CPUs.
	 */
	for_each_populated_zone(zone) {
		unsigned long flags, zflags;
		struct per_cpu_pages *pcp;

		pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);

		pcp_spin_lock_maybe_irqsave(pcp, flags);
		pcp->flags |= PCPF_CPU_DEAD;
		pcp_spin_unlock_maybe_irqrestore(pcp, flags);

		drain_pages_zone(cpu, zone);

		/*
		 * Drain released all pages. Reinitialize the
		 * owned-blocks list -- any remaining entries are
		 * stale (fragments that merged in zone buddy and
		 * cleared ownership, but weren't removed from
		 * the list because __free_one_page doesn't hold
		 * pcp->lock).
		 *
		 * Hold zone lock to prevent racing with other
		 * CPUs doing list_del_init on stale entries
		 * from this list during their Phase 1.
		 */
		pcp_spin_lock_maybe_irqsave(pcp, flags);
		spin_lock_irqsave(&zone->lock, zflags);
		INIT_LIST_HEAD(&pcp->owned_blocks);
		spin_unlock_irqrestore(&zone->lock, zflags);
		pcp_spin_unlock_maybe_irqrestore(pcp, flags);
	}

	/*
	 * Spill the event counters of the dead processor
	 * into the current processors event counters.
	 * This artificially elevates the count of the current
	 * processor.
	 */
	vm_events_fold_cpu(cpu);

	/*
	 * Zero the differential counters of the dead processor
	 * so that the vm statistics are consistent.
	 *
	 * This is only okay since the processor is dead and cannot
	 * race with what we are doing.
	 */
	cpu_vm_stats_fold(cpu);

	for_each_populated_zone(zone)
		zone_pcp_update(zone, 0);

	return 0;
}

static int page_alloc_cpu_online(unsigned int cpu)
{
	struct zone *zone;

	for_each_populated_zone(zone) {
		struct per_cpu_pages *pcp;
		unsigned long flags;

		pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);
		pcp_spin_lock_maybe_irqsave(pcp, flags);
		pcp->flags &= ~PCPF_CPU_DEAD;
		pcp_spin_unlock_maybe_irqrestore(pcp, flags);

		zone_pcp_update(zone, 1);
	}
	return 0;
}

void __init page_alloc_init_cpuhp(void)
{
	int ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_PAGE_ALLOC,
					"mm/page_alloc:pcp",
					page_alloc_cpu_online,
					page_alloc_cpu_dead);
	WARN_ON(ret < 0);
}

/*
 * calculate_totalreserve_pages - called when sysctl_lowmem_reserve_ratio
 *	or min_free_kbytes changes.
 */
static void calculate_totalreserve_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long reserve_pages = 0;
	enum zone_type i, j;

	for_each_online_pgdat(pgdat) {

		pgdat->totalreserve_pages = 0;

		for (i = 0; i < MAX_NR_ZONES; i++) {
			struct zone *zone = pgdat->node_zones + i;
			long max = 0;
			unsigned long managed_pages = zone_managed_pages(zone);

			/*
			 * lowmem_reserve[j] is monotonically non-decreasing
			 * in j for a given zone (see
			 * setup_per_zone_lowmem_reserve()). The maximum
			 * valid reserve lives at the highest index with a
			 * non-zero value, so scan backwards and stop at the
			 * first hit.
			 */
			for (j = MAX_NR_ZONES - 1; j > i; j--) {
				if (!zone->lowmem_reserve[j])
					continue;

				max = zone->lowmem_reserve[j];
				break;
			}
			/* we treat the high watermark as reserved pages. */
			max += high_wmark_pages(zone);

			max = min_t(unsigned long, max, managed_pages);

			pgdat->totalreserve_pages += max;

			reserve_pages += max;
		}
	}
	totalreserve_pages = reserve_pages;
	trace_mm_calculate_totalreserve_pages(totalreserve_pages);
}

/*
 * setup_per_zone_lowmem_reserve - called whenever
 *	sysctl_lowmem_reserve_ratio changes.  Ensures that each zone
 *	has a correct pages reserved value, so an adequate number of
 *	pages are left in the zone after a successful __alloc_pages().
 */
static void setup_per_zone_lowmem_reserve(void)
{
	struct pglist_data *pgdat;
	enum zone_type i, j;
	/*
	 * For a given zone node_zones[i], lowmem_reserve[j] (j > i)
	 * represents how many pages in zone i must effectively be kept
	 * in reserve when deciding whether an allocation class that is
	 * allowed to allocate from zones up to j may fall back into
	 * zone i.
	 *
	 * As j increases, the allocation class can use a strictly larger
	 * set of fallback zones and therefore must not be allowed to
	 * deplete low zones more aggressively than a less flexible one.
	 * As a result, lowmem_reserve[j] is required to be monotonically
	 * non-decreasing in j for each zone i. Callers such as
	 * calculate_totalreserve_pages() rely on this monotonicity when
	 * selecting the maximum reserve entry.
	 */
	for_each_online_pgdat(pgdat) {
		for (i = 0; i < MAX_NR_ZONES - 1; i++) {
			struct zone *zone = &pgdat->node_zones[i];
			int ratio = sysctl_lowmem_reserve_ratio[i];
			bool clear = !ratio || !zone_managed_pages(zone);
			unsigned long managed_pages = 0;

			for (j = i + 1; j < MAX_NR_ZONES; j++) {
				struct zone *upper_zone = &pgdat->node_zones[j];

				managed_pages += zone_managed_pages(upper_zone);

				if (clear)
					zone->lowmem_reserve[j] = 0;
				else
					zone->lowmem_reserve[j] = managed_pages / ratio;
				trace_mm_setup_per_zone_lowmem_reserve(zone, upper_zone,
								       zone->lowmem_reserve[j]);
			}
		}
	}

	/* update totalreserve_pages */
	calculate_totalreserve_pages();
}

static void __setup_per_zone_wmarks(void)
{
	unsigned long pages_min = min_free_kbytes >> (PAGE_SHIFT - 10);
	unsigned long lowmem_pages = 0;
	struct zone *zone;
	unsigned long flags;

	/* Calculate total number of !ZONE_HIGHMEM and !ZONE_MOVABLE pages */
	for_each_zone(zone) {
		if (!is_highmem(zone) && zone_idx(zone) != ZONE_MOVABLE)
			lowmem_pages += zone_managed_pages(zone);
	}

	for_each_zone(zone) {
		u64 tmp;

		spin_lock_irqsave(&zone->lock, flags);
		tmp = (u64)pages_min * zone_managed_pages(zone);
		tmp = div64_ul(tmp, lowmem_pages);
		if (is_highmem(zone) || zone_idx(zone) == ZONE_MOVABLE) {
			/*
			 * __GFP_HIGH and PF_MEMALLOC allocations usually don't
			 * need highmem and movable zones pages, so cap pages_min
			 * to a small  value here.
			 *
			 * The WMARK_HIGH-WMARK_LOW and (WMARK_LOW-WMARK_MIN)
			 * deltas control async page reclaim, and so should
			 * not be capped for highmem and movable zones.
			 */
			unsigned long min_pages;

			min_pages = zone_managed_pages(zone) / 1024;
			min_pages = clamp(min_pages, SWAP_CLUSTER_MAX, 128UL);
			zone->_watermark[WMARK_MIN] = min_pages;
		} else {
			/*
			 * If it's a lowmem zone, reserve a number of pages
			 * proportionate to the zone's size.
			 */
			zone->_watermark[WMARK_MIN] = tmp;
		}

		/*
		 * Set the kswapd watermarks distance according to the
		 * scale factor in proportion to available memory, but
		 * ensure a minimum size on small systems.
		 */
		tmp = max_t(u64, tmp >> 2,
			    mult_frac(zone_managed_pages(zone),
				      watermark_scale_factor, 10000));

		zone->_watermark[WMARK_LOW]  = min_wmark_pages(zone) + tmp;
		zone->_watermark[WMARK_HIGH] = low_wmark_pages(zone) + tmp;
		zone->_watermark[WMARK_PROMO] = high_wmark_pages(zone) + tmp;
		trace_mm_setup_per_zone_wmarks(zone);

		spin_unlock_irqrestore(&zone->lock, flags);
	}

	/* update totalreserve_pages */
	calculate_totalreserve_pages();
}

/**
 * setup_per_zone_wmarks - called when min_free_kbytes changes
 * or when memory is hot-{added|removed}
 *
 * Ensures that the watermark[min,low,high] values for each zone are set
 * correctly with respect to min_free_kbytes.
 */
void setup_per_zone_wmarks(void)
{
	struct zone *zone;
	static DEFINE_SPINLOCK(lock);

	spin_lock(&lock);
	__setup_per_zone_wmarks();
	spin_unlock(&lock);

	/*
	 * The watermark size have changed so update the pcpu batch
	 * and high limits or the limits may be inappropriate.
	 */
	for_each_zone(zone)
		zone_pcp_update(zone, 0);
}

/*
 * Initialise min_free_kbytes.
 *
 * For small machines we want it small (128k min).  For large machines
 * we want it large (256MB max).  But it is not linear, because network
 * bandwidth does not increase linearly with machine size.  We use
 *
 *	min_free_kbytes = 4 * sqrt(lowmem_kbytes), for better accuracy:
 *	min_free_kbytes = sqrt(lowmem_kbytes * 16)
 *
 * which yields
 *
 * 16MB:	512k
 * 32MB:	724k
 * 64MB:	1024k
 * 128MB:	1448k
 * 256MB:	2048k
 * 512MB:	2896k
 * 1024MB:	4096k
 * 2048MB:	5792k
 * 4096MB:	8192k
 * 8192MB:	11584k
 * 16384MB:	16384k
 */
void calculate_min_free_kbytes(void)
{
	unsigned long lowmem_kbytes;
	int new_min_free_kbytes;

	lowmem_kbytes = nr_free_buffer_pages() * (PAGE_SIZE >> 10);
	new_min_free_kbytes = int_sqrt(lowmem_kbytes * 16);

	if (new_min_free_kbytes > user_min_free_kbytes)
		min_free_kbytes = clamp(new_min_free_kbytes, 128, 262144);
	else
		pr_warn("min_free_kbytes is not updated to %d because user defined value %d is preferred\n",
				new_min_free_kbytes, user_min_free_kbytes);

}

int __meminit init_per_zone_wmark_min(void)
{
	calculate_min_free_kbytes();
	setup_per_zone_wmarks();
	refresh_zone_stat_thresholds();
	setup_per_zone_lowmem_reserve();

#ifdef CONFIG_NUMA
	setup_min_unmapped_ratio();
	setup_min_slab_ratio();
#endif

	khugepaged_min_free_kbytes_update();

	return 0;
}
postcore_initcall(init_per_zone_wmark_min)

/*
 * min_free_kbytes_sysctl_handler - just a wrapper around proc_dointvec() so
 *	that we can call two helper functions whenever min_free_kbytes
 *	changes.
 */
static int min_free_kbytes_sysctl_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	if (write) {
		user_min_free_kbytes = min_free_kbytes;
		setup_per_zone_wmarks();
	}
	return 0;
}

static int watermark_scale_factor_sysctl_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	if (write)
		setup_per_zone_wmarks();

	return 0;
}

#ifdef CONFIG_NUMA
static void setup_min_unmapped_ratio(void)
{
	pg_data_t *pgdat;
	struct zone *zone;

	for_each_online_pgdat(pgdat)
		pgdat->min_unmapped_pages = 0;

	for_each_zone(zone)
		zone->zone_pgdat->min_unmapped_pages += (zone_managed_pages(zone) *
						         sysctl_min_unmapped_ratio) / 100;
}


static int sysctl_min_unmapped_ratio_sysctl_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	setup_min_unmapped_ratio();

	return 0;
}

static void setup_min_slab_ratio(void)
{
	pg_data_t *pgdat;
	struct zone *zone;

	for_each_online_pgdat(pgdat)
		pgdat->min_slab_pages = 0;

	for_each_zone(zone)
		zone->zone_pgdat->min_slab_pages += (zone_managed_pages(zone) *
						     sysctl_min_slab_ratio) / 100;
}

static int sysctl_min_slab_ratio_sysctl_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	setup_min_slab_ratio();

	return 0;
}
#endif

/*
 * lowmem_reserve_ratio_sysctl_handler - just a wrapper around
 *	proc_dointvec() so that we can call setup_per_zone_lowmem_reserve()
 *	whenever sysctl_lowmem_reserve_ratio changes.
 *
 * The reserve ratio obviously has absolutely no relation with the
 * minimum watermarks. The lowmem reserve ratio can only make sense
 * if in function of the boot time zone sizes.
 */
static int lowmem_reserve_ratio_sysctl_handler(const struct ctl_table *table,
		int write, void *buffer, size_t *length, loff_t *ppos)
{
	int i;

	proc_dointvec_minmax(table, write, buffer, length, ppos);

	for (i = 0; i < MAX_NR_ZONES; i++) {
		if (sysctl_lowmem_reserve_ratio[i] < 1)
			sysctl_lowmem_reserve_ratio[i] = 0;
	}

	setup_per_zone_lowmem_reserve();
	return 0;
}

/*
 * percpu_pagelist_high_fraction - changes the pcp->high for each zone on each
 * cpu. It is the fraction of total pages in each zone that a hot per cpu
 * pagelist can have before it gets flushed back to buddy allocator.
 */
static int percpu_pagelist_high_fraction_sysctl_handler(const struct ctl_table *table,
		int write, void *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	int old_percpu_pagelist_high_fraction;
	int ret;

	/*
	 * Avoid using pcp_batch_high_lock for reads as the value is read
	 * atomically and a race with offlining is harmless.
	 */

	if (!write)
		return proc_dointvec_minmax(table, write, buffer, length, ppos);

	mutex_lock(&pcp_batch_high_lock);
	old_percpu_pagelist_high_fraction = percpu_pagelist_high_fraction;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret < 0)
		goto out;

	/* Sanity checking to avoid pcp imbalance */
	if (percpu_pagelist_high_fraction &&
	    percpu_pagelist_high_fraction < MIN_PERCPU_PAGELIST_HIGH_FRACTION) {
		percpu_pagelist_high_fraction = old_percpu_pagelist_high_fraction;
		ret = -EINVAL;
		goto out;
	}

	/* No change? */
	if (percpu_pagelist_high_fraction == old_percpu_pagelist_high_fraction)
		goto out;

	for_each_populated_zone(zone)
		zone_set_pageset_high_and_batch(zone, 0);
out:
	mutex_unlock(&pcp_batch_high_lock);
	return ret;
}

static const struct ctl_table page_alloc_sysctl_table[] = {
	{
		.procname	= "min_free_kbytes",
		.data		= &min_free_kbytes,
		.maxlen		= sizeof(min_free_kbytes),
		.mode		= 0644,
		.proc_handler	= min_free_kbytes_sysctl_handler,
		.extra1		= SYSCTL_ZERO,
	},
	{
		.procname	= "watermark_scale_factor",
		.data		= &watermark_scale_factor,
		.maxlen		= sizeof(watermark_scale_factor),
		.mode		= 0644,
		.proc_handler	= watermark_scale_factor_sysctl_handler,
		.extra1		= SYSCTL_ONE,
		.extra2		= SYSCTL_THREE_THOUSAND,
	},
	{
		.procname	= "defrag_mode",
		.data		= &defrag_mode,
		.maxlen		= sizeof(defrag_mode),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "percpu_pagelist_high_fraction",
		.data		= &percpu_pagelist_high_fraction,
		.maxlen		= sizeof(percpu_pagelist_high_fraction),
		.mode		= 0644,
		.proc_handler	= percpu_pagelist_high_fraction_sysctl_handler,
		.extra1		= SYSCTL_ZERO,
	},
	{
		.procname	= "lowmem_reserve_ratio",
		.data		= &sysctl_lowmem_reserve_ratio,
		.maxlen		= sizeof(sysctl_lowmem_reserve_ratio),
		.mode		= 0644,
		.proc_handler	= lowmem_reserve_ratio_sysctl_handler,
	},
#ifdef CONFIG_NUMA
	{
		.procname	= "numa_zonelist_order",
		.data		= &numa_zonelist_order,
		.maxlen		= NUMA_ZONELIST_ORDER_LEN,
		.mode		= 0644,
		.proc_handler	= numa_zonelist_order_handler,
	},
	{
		.procname	= "min_unmapped_ratio",
		.data		= &sysctl_min_unmapped_ratio,
		.maxlen		= sizeof(sysctl_min_unmapped_ratio),
		.mode		= 0644,
		.proc_handler	= sysctl_min_unmapped_ratio_sysctl_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE_HUNDRED,
	},
	{
		.procname	= "min_slab_ratio",
		.data		= &sysctl_min_slab_ratio,
		.maxlen		= sizeof(sysctl_min_slab_ratio),
		.mode		= 0644,
		.proc_handler	= sysctl_min_slab_ratio_sysctl_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE_HUNDRED,
	},
#endif
};

void __init page_alloc_sysctl_init(void)
{
	register_sysctl_init("vm", page_alloc_sysctl_table);
}

#ifdef CONFIG_COMPACTION
/*
 * Pageblock evacuation: synchronously migrate movable pages out of a
 * pageblock to consolidate fragmentation. Driven by the background
 * superpageblock defragmentation worker (see below); has no per-pageblock
 * scheduling infrastructure of its own.
 */

static void evacuate_pageblock(struct zone *zone, unsigned long start_pfn,
			       bool force)
{
	unsigned long end_pfn = start_pfn + pageblock_nr_pages;
	unsigned long pfn = start_pfn;
	int nr_reclaimed;
	int ret = 0;
	struct compact_control cc = {
		.nr_migratepages = 0,
		.order = -1,
		.zone = zone,
		.mode = MIGRATE_ASYNC,
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
	};
	struct migration_target_control mtc = {
		.nid = zone_to_nid(zone),
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
	};

	/*
	 * Verify this pageblock is still worth evacuating.
	 * Skip if it reverted to MOVABLE (steal was undone) — unless
	 * force is set (background defrag wants to clear movable pages
	 * out of tainted superpageblocks regardless of pageblock type).
	 */
	if (!force &&
	    get_pageblock_migratetype(pfn_to_page(start_pfn)) == MIGRATE_MOVABLE)
		return;

	INIT_LIST_HEAD(&cc.migratepages);

	/*
	 * Loop through the entire pageblock, isolating and migrating
	 * in batches. isolate_migratepages_range stops at
	 * COMPACT_CLUSTER_MAX, so we must loop to cover the full block.
	 */
	while (pfn < end_pfn || !list_empty(&cc.migratepages)) {
		if (list_empty(&cc.migratepages)) {
			cc.nr_migratepages = 0;
			cc.migrate_pfn = pfn;
			ret = isolate_migratepages_range(&cc, pfn, end_pfn);
			if (ret && ret != -EAGAIN)
				break;
			pfn = cc.migrate_pfn;
			if (list_empty(&cc.migratepages))
				break;
		}

		nr_reclaimed = reclaim_clean_pages_from_list(zone,
							&cc.migratepages);
		cc.nr_migratepages -= nr_reclaimed;

		if (!list_empty(&cc.migratepages)) {
			ret = migrate_pages(&cc.migratepages,
					    alloc_migration_target, NULL,
					    (unsigned long)&mtc, cc.mode,
					    MR_COMPACTION, NULL);
			if (ret) {
				putback_movable_pages(&cc.migratepages);
				break;
			}
		}

		cond_resched();
	}

	if (!list_empty(&cc.migratepages))
		putback_movable_pages(&cc.migratepages);

	/*
	 * Re-scan to let isolate_migratepages_block clear PB_has_movable
	 * if no movable pages remain after evacuation.
	 */
	cc.migrate_pfn = start_pfn;
	cc.nr_migratepages = 0;
	INIT_LIST_HEAD(&cc.migratepages);
	isolate_migratepages_range(&cc, start_pfn, end_pfn);
	if (!list_empty(&cc.migratepages))
		putback_movable_pages(&cc.migratepages);
}

/*
 * Atomic-context SPB evacuation deferral.
 *
 * When an atomic allocation in __rmqueue_claim is about to taint a
 * clean superpageblock because the tainted pool has no free page at
 * the requested (order, migratetype), schedule a deferred call to
 * spb_evacuate_for_order. That frees pageblocks inside tainted SPBs so
 * subsequent allocations can claim them instead of tainting more clean
 * SPBs.
 *
 * Two-step deferral mirrors the pageblock-evacuate path: irq_work to
 * leave allocator lock context, then queue_work to reach process
 * context where spb_evacuate_for_order can sleep in migrate_pages.
 */

struct spb_evac_request {
	struct work_struct	work;
	struct zone		*zone;
	unsigned int		order;
	int			migratetype;
	struct llist_node	free_node;
};

#define NR_SPB_EVAC_REQUESTS	64
static struct spb_evac_request spb_evac_pool[NR_SPB_EVAC_REQUESTS];
static struct llist_head spb_evac_freelist;

static struct spb_evac_request *spb_evac_request_alloc(void)
{
	struct llist_node *node;

	node = llist_del_first(&spb_evac_freelist);
	if (!node)
		return NULL;
	return container_of(node, struct spb_evac_request, free_node);
}

static void spb_evac_request_free(struct spb_evac_request *req)
{
	llist_add(&req->free_node, &spb_evac_freelist);
}

static void spb_evac_work_fn(struct work_struct *work)
{
	struct spb_evac_request *req = container_of(work,
						    struct spb_evac_request,
						    work);
	struct zone *zone = req->zone;
	unsigned int order = req->order;
	int mt = req->migratetype;

	spb_evacuate_for_order(zone, order, mt);

	/*
	 * Clearing the in-flight bit lets a future caller hitting the
	 * same (mt, order) re-enqueue evacuation.  Ordering between this
	 * worker's SPB state changes and the future caller's
	 * tainted_pool_has_free walk is provided by zone->lock taken
	 * inside spb_evacuate_for_order and by the future caller.
	 */
	clear_bit(mt * NR_PAGE_ORDERS + order, zone->spb_evac_in_flight);
	spb_evac_request_free(req);
}

static void spb_evac_irq_work_fn(struct irq_work *work)
{
	pg_data_t *pgdat = container_of(work, pg_data_t,
					spb_evac_irq_work);
	struct llist_node *pending;
	struct spb_evac_request *req, *next;

	if (!pgdat->evacuate_wq)
		return;

	pending = llist_del_all(&pgdat->spb_evac_pending);
	llist_for_each_entry_safe(req, next, pending, free_node) {
		INIT_WORK(&req->work, spb_evac_work_fn);
		queue_work(pgdat->evacuate_wq, &req->work);
	}
}

/*
 * Walk tainted SPBs to check whether any has a free page at the given
 * order and migratetype.  When this returns true, a clean-SPB claim is
 * not pool depletion but a try_to_claim_block over-rejection: skip the
 * deferred evacuation since it cannot help.
 */
static bool tainted_pool_has_free(struct zone *zone, unsigned int order,
				  int migratetype)
{
	struct superpageblock *sb;
	int full;

	lockdep_assert_held(&zone->lock);

	for (full = 0; full < __NR_SB_FULLNESS; full++) {
		list_for_each_entry(sb, &zone->spb_lists[SB_TAINTED][full],
				    list) {
			struct free_area *fa = &sb->free_area[order];

			if (fa->nr_free &&
			    !list_empty(&fa->free_list[migratetype]))
				return true;
		}
	}
	return false;
}

/**
 * queue_spb_evacuate - schedule deferred SPB evacuation from atomic context
 * @zone: zone that just failed to find a free page in the tainted pool
 * @order: requested allocation order
 * @migratetype: requested migratetype (UNMOVABLE or RECLAIMABLE only)
 *
 * Caller must hold zone->lock; the tainted-pool walk asserts it.
 *
 * Single-flight gated per (zone, migratetype, order) and throttled to
 * one enqueue per 10ms per (zone, migratetype).  Pool exhaustion
 * silently drops the request; the next caller hitting the same trigger
 * will retry.
 */
static void queue_spb_evacuate(struct zone *zone, unsigned int order,
			       int migratetype)
{
	pg_data_t *pgdat = zone->zone_pgdat;
	struct spb_evac_request *req;
	unsigned int bit;

	lockdep_assert_held(&zone->lock);

	if (!pgdat->spb_evac_irq_work.func)
		return;
	if (order >= NR_PAGE_ORDERS || migratetype >= MIGRATE_PCPTYPES)
		return;

	if (time_before(jiffies,
			zone->spb_evac_last[migratetype] + HZ / 100))
		return;

	bit = migratetype * NR_PAGE_ORDERS + order;
	if (test_and_set_bit(bit, zone->spb_evac_in_flight))
		return;

	if (tainted_pool_has_free(zone, order, migratetype)) {
		clear_bit(bit, zone->spb_evac_in_flight);
		return;
	}

	req = spb_evac_request_alloc();
	if (!req) {
		clear_bit(bit, zone->spb_evac_in_flight);
		return;
	}

	zone->spb_evac_last[migratetype] = jiffies;
	req->zone = zone;
	req->order = order;
	req->migratetype = migratetype;
	llist_add(&req->free_node, &pgdat->spb_evac_pending);
	irq_work_queue(&pgdat->spb_evac_irq_work);
}

/*
 * SPB-driven slab reclaim.
 *
 * When tainted SPBs run low on free pageblocks under sustained
 * non-movable pressure (slab inode/dentry/page-table caches), the
 * pageblock-evacuation worker can only consolidate *movable* pages out
 * of tainted SPBs. Non-movable slab content stays put, so once the
 * movable supply is drained the only way to recover headroom in a
 * tainted SPB is to shrink the slab caches whose pages live there.
 *
 * shrink_slab() is node-scoped, so one work item per pgdat is enough:
 * a single embedded work_struct. queue_work() returns false if the work
 * is already queued/running, so we get single-flight for free — fresh
 * triggers no-op until the in-flight pass completes.
 *
 * shrink_slab() itself is location-agnostic — it walks all registered
 * shrinkers and frees objects whose backing pages may live in any
 * zone or SPB. That is fine here because any slab page reclaimed
 * frees space the next allocation can reuse without tainting a fresh
 * SPB. We pass the pgdat's nid so node-aware shrinkers prefer caches
 * local to the pressured node.
 */

/*
 * Per-invocation budget: walk shrinkers from DEF_PRIORITY (scan 1/4096
 * of each cache) down toward 0 (full scan), stopping when shrinkers
 * report no more progress or we have freed a pageblock-sized chunk.
 * The trigger frequency is what controls overall reclaim rate; this
 * loop just bounds latency per worker run.
 */
#define SPB_SLAB_SHRINK_TARGET_OBJS	(pageblock_nr_pages * 4UL)

static void spb_slab_shrink_work_fn(struct work_struct *work)
{
	pg_data_t *pgdat = container_of(work, pg_data_t,
					spb_slab_shrink_work);
	int nid = pgdat->node_id;
	unsigned long freed = 0;
	int prio = DEF_PRIORITY;

	count_vm_event(SPB_SLAB_SHRINK_RAN);

	while (freed < SPB_SLAB_SHRINK_TARGET_OBJS && prio >= 0) {
		unsigned long delta = 0;
		struct mem_cgroup *memcg;

		/*
		 * Walk the memcg hierarchy starting at the root, the same
		 * pattern shrink_one_node uses for global slab reclaim.
		 * Some cgroups may not be present on the node that is
		 * being shrunk, but many allocators will use any memory.
		 */
		memcg = mem_cgroup_iter(NULL, NULL, NULL);
		do {
			delta += shrink_slab(GFP_KERNEL, nid, memcg, prio);
		} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);

		if (!delta)
			break;
		freed += delta;
		/*
		 * Increase aggressiveness each round; DEF_PRIORITY scans
		 * a small slice of each cache, prio 0 scans the whole
		 * thing. Most workloads find enough at one or two
		 * iterations below DEF_PRIORITY.
		 */
		prio--;
	}
}

/**
 * queue_spb_slab_shrink - schedule deferred slab shrink for SPB pressure
 * @zone: zone whose tainted-SPB pool is running low
 *
 * Single-flight via queue_work(): if the work is already queued or
 * running, it returns false and we no-op. There is no time-based
 * throttle — the rate at which fresh shrink runs can fire is bounded
 * by how fast the worker completes (one full pass freeing up to
 * SPB_SLAB_SHRINK_TARGET_OBJS objects).
 *
 * Callable from any context: page allocator paths hold zone->lock,
 * the SPB evacuate worker does not. queue_work() takes only the
 * workqueue's pool lock — no zone->lock dependency.
 *
 * Pairs with queue_spb_evacuate: evacuation moves movable pages out
 * of tainted SPBs to free up whole pageblocks; this shrinks slab to
 * free up the remaining (non-movable) pages. We queue both because
 * even when movable evacuation succeeds, shrinking slab in parallel
 * keeps headroom available for the next burst, when movable supply
 * may have run out.
 */
static void queue_spb_slab_shrink(struct zone *zone)
{
	pg_data_t *pgdat = zone->zone_pgdat;

	if (!pgdat->evacuate_wq)
		return;

	if (queue_work(pgdat->evacuate_wq, &pgdat->spb_slab_shrink_work))
		count_vm_event(SPB_SLAB_SHRINK_QUEUED);
}

/*
 * Background superpageblock defragmentation.
 *
 * Evacuate movable pageblocks from tainted superpageblocks to consolidate
 * contamination. Triggered on-demand when a tainted superpageblock runs
 * low on free space, rather than running on a fixed timer.
 *
 * Goals for tainted superpageblocks:
 * - At least 2 free pageblocks if movable pageblocks still exist
 * - Or 3 pageblocks worth of free pages while movable pages remain
 * - Skip superpageblocks with no movable pages (nothing to evacuate)
 */

/*
 * Target free space for clean SPB internal compaction: at least a quarter
 * of the superpageblock must be free before we attempt to consolidate
 * scattered free pages into whole free pageblocks. Below this threshold
 * the work-to-payoff ratio is poor — we walk the whole SPB and migrate
 * a handful of pages without producing a usable free pageblock.
 */
#define SPB_DEFRAG_FREE_PAGES_TARGET	(SUPERPAGEBLOCK_NR_PAGES / 4)

/**
 * spb_needs_defrag - Check if a superpageblock needs defragmentation
 * @sb: superpageblock to check (may be NULL)
 *
 * For tainted superpageblocks: defrag is needed when there are movable
 * pageblocks that can be evacuated AND free space is running low.
 * For clean superpageblocks: compaction is needed when free pages are
 * scattered (plenty of free pages but few whole free pageblocks).
 */
/*
 * Cooldown between defrag attempts that made no progress, in seconds.
 * Long enough to keep the allocator hot path quiet on saturated SBs;
 * short enough that a freshly-freed pageblock isn't ignored for long.
 */
#define SPB_DEFRAG_NOOP_COOLDOWN_SECS	5

static bool spb_needs_defrag(struct superpageblock *sb)
{
	if (!sb)
		return false;

	/*
	 * Back off if the previous pass made no progress: do not retry until
	 * either the cooldown elapses or free pages have grown by at least a
	 * pageblock's worth (a hint that there might be new material to
	 * consolidate or evacuate).
	 */
	if (sb->defrag_last_no_progress_jiffies &&
	    time_before(jiffies, sb->defrag_last_no_progress_jiffies +
				 SPB_DEFRAG_NOOP_COOLDOWN_SECS * HZ) &&
	    sb->nr_free_pages < sb->defrag_last_no_progress_pages +
				pageblock_nr_pages)
		return false;

	/*
	 * Tainted superpageblocks: evacuate movable pages to concentrate
	 * unmovable/reclaimable allocations.  Migration targets are
	 * allocated system-wide, so no internal free space is needed.
	 * Maintain the tainted reserve so unmovable claims always
	 * find room in existing tainted superpageblocks.
	 */
	if (spb_get_category(sb) == SB_TAINTED)
		return sb->nr_movable > 0 &&
		       sb->nr_free < spb_tainted_reserve(sb);

	/*
	 * Clean superpageblocks: compact scattered free pages into whole
	 * free pageblocks.  Needs internal free space as destination.
	 */
	if (sb->nr_free >= 2)
		return false;

	if (sb->nr_free_pages < SPB_DEFRAG_FREE_PAGES_TARGET)
		return false;

	return true;
}

/**
 * spb_defrag_done - Check if defrag/compaction should stop
 * @sb: superpageblock being defragmented
 *
 * Stop when the superpageblock has enough free pageblocks, when free
 * pages drop too low to be worth continuing, or (for tainted
 * superpageblocks) when there are no more movable pages to evacuate.
 */
static bool spb_defrag_done(struct superpageblock *sb)
{
	/*
	 * Tainted superpageblocks: keep evacuating movable pages until
	 * the reserve of free pageblocks is restored, or until there
	 * are no more movable pages to evacuate.
	 */
	if (spb_get_category(sb) == SB_TAINTED)
		return !sb->nr_movable ||
		       sb->nr_free >= spb_tainted_reserve(sb);

	/* Clean superpageblocks: stop when enough free pageblocks exist */
	if (sb->nr_free >= 2)
		return true;

	if (sb->nr_free_pages < SPB_DEFRAG_FREE_PAGES_TARGET)
		return true;

	return false;
}

static void spb_clear_skip_bits(struct superpageblock *sb)
{
	unsigned long pfn, end_pfn;
	struct zone *zone = sb->zone;

	end_pfn = sb->start_pfn + SUPERPAGEBLOCK_NR_PAGES;

	for (pfn = sb->start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		struct page *page;

		if (!pfn_valid(pfn))
			continue;
		if (!zone_spans_pfn(zone, pfn))
			continue;

		page = pfn_to_page(pfn);
		clear_pageblock_skip(page);
	}
}

/**
 * spb_defrag_tainted - evacuate movable pages from a tainted superpageblock
 * @sb: the tainted superpageblock to defragment
 *
 * Find any pageblock with movable pages (PB_has_movable) and evacuate
 * them, leaving only unmovable, reclaimable, and free pages behind.
 * Stop when the free space target is reached.
 */
static void spb_defrag_tainted(struct superpageblock *sb)
{
	unsigned long pfn, end_pfn, start_pfn, cursor;
	struct zone *zone = sb->zone;
	bool wrapped = false;

	if (!sb->nr_movable)
		return;

	start_pfn = sb->start_pfn;
	end_pfn = start_pfn + SUPERPAGEBLOCK_NR_PAGES;

	cursor = sb->defrag_cursor;
	if (cursor < start_pfn || cursor >= end_pfn) {
		cursor = start_pfn;
		spb_clear_skip_bits(sb);
	}

	pfn = cursor;

	while (pfn < end_pfn) {
		struct page *page;

		if (spb_defrag_done(sb))
			goto out;

		if (!pfn_valid(pfn))
			goto next;

		if (!zone_spans_pfn(zone, pfn))
			goto next;

		page = pfn_to_page(pfn);

		if (!get_pfnblock_bit(page, pfn, PB_has_movable))
			goto next;

		if (get_pfnblock_bit(page, pfn, PB_all_free))
			goto next;

		if (get_pageblock_skip(page))
			goto next;

		evacuate_pageblock(zone, pfn, true);
next:
		pfn += pageblock_nr_pages;
		if (pfn >= end_pfn && !wrapped) {
			spb_clear_skip_bits(sb);
			pfn = start_pfn;
			wrapped = true;
		}
		if (wrapped && pfn > cursor)
			break;
	}
out:
	sb->defrag_cursor = pfn;
}

/*
 * Within-superpageblock compaction: migrate pages from partially-used
 * pageblocks into free space within the same superpageblock, consolidating
 * scattered free pages into whole free pageblocks.
 */

struct spb_compaction_control {
	struct superpageblock	*sb;
	struct zone		*zone;
};

/*
 * alloc_spb_compaction_target - allocate a migration target page from
 * within the same superpageblock's free lists.
 *
 * This is a custom migration target allocator that restricts allocations
 * to the superpageblock being compacted, ensuring pages stay within the SB.
 */
static struct folio *alloc_spb_compaction_target(struct folio *src,
		unsigned long private)
{
	struct spb_compaction_control *scc =
		(struct spb_compaction_control *)private;
	struct superpageblock *sb = scc->sb;
	struct zone *zone = scc->zone;
	int src_order = folio_order(src);
	int order = src_order;
	int migratetype = MIGRATE_MOVABLE;
	struct free_area *area;
	struct page *target;

	spin_lock_irq(&zone->lock);

	area = &sb->free_area[order];
	target = get_page_from_free_area(area, migratetype);
	if (!target) {
		/* Try to split a higher-order block within this SB */
		for (order = src_order + 1; order < NR_PAGE_ORDERS; order++) {
			area = &sb->free_area[order];
			target = get_page_from_free_area(area, migratetype);
			if (target)
				break;
		}
	}

	if (target)
		page_del_and_expand(zone, target, src_order, order, migratetype);

	spin_unlock_irq(&zone->lock);

	if (!target)
		return NULL;

	prep_new_page(target, src_order, __GFP_MOVABLE | __GFP_COMP, 0);
	set_page_refcounted(target);
	return page_rmappable_folio(target);
}

static void free_spb_compaction_target(struct folio *folio,
		unsigned long private)
{
	folio_put(folio);
}

/*
 * compact_pageblock_in_spb - migrate pages from a partially-used pageblock
 * into free space within the same superpageblock.
 *
 * Similar to evacuate_pageblock() but uses the within-SB allocator
 * so pages stay inside the superpageblock being compacted.
 */
static void compact_pageblock_in_spb(struct superpageblock *sb,
				    struct zone *zone,
				    unsigned long start_pfn)
{
	unsigned long end_pfn = start_pfn + pageblock_nr_pages;
	unsigned long pfn = start_pfn;
	int nr_reclaimed;
	int ret = 0;
	struct compact_control cc = {
		.nr_migratepages = 0,
		.order = -1,
		.zone = zone,
		.mode = MIGRATE_SYNC_LIGHT,
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
	};
	struct spb_compaction_control scc = {
		.sb = sb,
		.zone = zone,
	};

	INIT_LIST_HEAD(&cc.migratepages);

	while (pfn < end_pfn || !list_empty(&cc.migratepages)) {
		if (list_empty(&cc.migratepages)) {
			cc.nr_migratepages = 0;
			cc.migrate_pfn = pfn;
			ret = isolate_migratepages_range(&cc, pfn, end_pfn);
			if (ret && ret != -EAGAIN)
				break;
			pfn = cc.migrate_pfn;
			if (list_empty(&cc.migratepages))
				break;
		}

		nr_reclaimed = reclaim_clean_pages_from_list(zone,
							&cc.migratepages);
		cc.nr_migratepages -= nr_reclaimed;

		if (!list_empty(&cc.migratepages)) {
			ret = migrate_pages(&cc.migratepages,
					    alloc_spb_compaction_target,
					    free_spb_compaction_target,
					    (unsigned long)&scc, cc.mode,
					    MR_COMPACTION, NULL);
			if (ret) {
				putback_movable_pages(&cc.migratepages);
				break;
			}
		}

		cond_resched();
	}

	if (!list_empty(&cc.migratepages))
		putback_movable_pages(&cc.migratepages);
}

/**
 * spb_defrag_clean - compact a clean superpageblock internally
 * @sb: the clean superpageblock to compact
 *
 * Scan pageblocks in the superpageblock looking for partially-used ones.
 * Skip fully free pageblocks and pageblocks recently marked unsuitable
 * by the pageblock_skip bit; PCPBuddy-cached pages within an otherwise
 * compactable pageblock are skipped per-page by isolate_migratepages_block().
 * Migrate pages from the best candidate into free space within the same
 * superpageblock.
 */
static void spb_defrag_clean(struct superpageblock *sb)
{
	unsigned long pfn, end_pfn, start_pfn, cursor;
	struct zone *zone = sb->zone;
	bool wrapped = false;

	start_pfn = sb->start_pfn;
	end_pfn = start_pfn + SUPERPAGEBLOCK_NR_PAGES;

	cursor = sb->defrag_cursor;
	if (cursor < start_pfn || cursor >= end_pfn) {
		cursor = start_pfn;
		spb_clear_skip_bits(sb);
	}

	pfn = cursor;

	while (pfn < end_pfn) {
		struct page *page;

		if (spb_defrag_done(sb))
			goto out;

		if (!pfn_valid(pfn))
			goto next;

		if (!zone_spans_pfn(zone, pfn))
			goto next;

		page = pfn_to_page(pfn);

		if (get_pfnblock_bit(page, pfn, PB_all_free))
			goto next;

		if (get_pageblock_skip(page))
			goto next;

		compact_pageblock_in_spb(sb, zone, pfn);
next:
		pfn += pageblock_nr_pages;
		if (pfn >= end_pfn && !wrapped) {
			spb_clear_skip_bits(sb);
			pfn = start_pfn;
			wrapped = true;
		}
		if (wrapped && pfn > cursor)
			break;
	}
out:
	sb->defrag_cursor = pfn;
}

/**
 * spb_defrag_superpageblock - defragment a superpageblock
 * @sb: the superpageblock to defragment
 *
 * Dispatch to the appropriate defrag strategy based on superpageblock
 * category: evacuate movable pages from tainted superpageblocks, or
 * compact scattered free pages within clean superpageblocks.
 */
static void spb_defrag_superpageblock(struct superpageblock *sb)
{
	if (spb_get_category(sb) == SB_TAINTED)
		spb_defrag_tainted(sb);
	else
		spb_defrag_clean(sb);
}

static void spb_defrag_work_fn(struct work_struct *work)
{
	struct superpageblock *sb = container_of(work, struct superpageblock,
					     defrag_work);
	u16 nr_free_before = sb->nr_free;

	spb_defrag_superpageblock(sb);

	/*
	 * If this pass produced no new free pageblocks, arm the no-progress
	 * cooldown so spb_needs_defrag() rejects re-arms until either time
	 * passes or nr_free_pages grows enough to suggest new material to
	 * work on.  Use jiffies | 1 so the field is never accidentally zero.
	 */
	if (sb->nr_free == nr_free_before) {
		sb->defrag_last_no_progress_jiffies = jiffies | 1;
		sb->defrag_last_no_progress_pages = sb->nr_free_pages;
	} else {
		sb->defrag_last_no_progress_jiffies = 0;
	}

	/* Allow new defrag requests for this superpageblock */
	sb->defrag_active = false;
}

/**
 * spb_defrag_irq_work_fn - IRQ work callback to safely queue defrag work
 * @work: the irq_work embedded in struct superpageblock
 *
 * queue_work() can deadlock when called from inside the page allocator
 * because it may try to allocate memory with locks already held.
 * Use irq_work to defer the queue_work() call to a safe context.
 */
static void spb_defrag_irq_work_fn(struct irq_work *work)
{
	struct superpageblock *sb = container_of(work, struct superpageblock,
					     defrag_irq_work);
	pg_data_t *pgdat = sb->zone->zone_pgdat;

	if (pgdat->evacuate_wq)
		queue_work(pgdat->evacuate_wq, &sb->defrag_work);
}

/**
 * spb_maybe_start_defrag - Trigger defrag if a superpageblock needs it
 * @sb: superpageblock whose counters just changed
 *
 * Called from counter update paths (under zone->lock). If the
 * superpageblock needs defragmentation — either evacuation of movable
 * pages from a tainted superpageblock, or internal compaction of a
 * clean superpageblock — schedule irq_work to queue defrag work outside
 * the allocator's lock context. The irq_work handler is set up by
 * pageblock_evacuate_init(); before that runs, defrag_irq_work.func
 * is NULL and we skip.
 */
static void spb_maybe_start_defrag(struct superpageblock *sb)
{
	if (!spb_needs_defrag(sb))
		return;

	/* Don't pile up work items; one defrag pass per superpageblock at a time */
	if (sb->defrag_active)
		return;

	if (sb->defrag_irq_work.func) {
		sb->defrag_active = true;
		irq_work_queue(&sb->defrag_irq_work);
	}
}

static int __init pageblock_evacuate_init(void)
{
	int nid, i;

	/* Initialize the global freelist of SPB evacuate requests */
	init_llist_head(&spb_evac_freelist);
	for (i = 0; i < NR_SPB_EVAC_REQUESTS; i++)
		llist_add(&spb_evac_pool[i].free_node, &spb_evac_freelist);


	/* Create a per-pgdat workqueue */
	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		char name[32];
		int z;

		snprintf(name, sizeof(name), "kevacuate/%d", nid);
		pgdat->evacuate_wq = alloc_workqueue(name, WQ_MEM_RECLAIM, 1);
		if (!pgdat->evacuate_wq) {
			pr_warn("Failed to create evacuate workqueue for node %d\n", nid);
			continue;
		}

		init_llist_head(&pgdat->spb_evac_pending);
		init_irq_work(&pgdat->spb_evac_irq_work,
			      spb_evac_irq_work_fn);

		INIT_WORK(&pgdat->spb_slab_shrink_work,
			  spb_slab_shrink_work_fn);

		/* Initialize per-superpageblock defrag work structs */
		for (z = 0; z < MAX_NR_ZONES; z++) {
			struct zone *zone = &pgdat->node_zones[z];
			unsigned long j;

			if (!zone->superpageblocks)
				continue;

			for (j = 0; j < zone->nr_superpageblocks; j++) {
				INIT_WORK(&zone->superpageblocks[j].defrag_work,
					  spb_defrag_work_fn);
				init_irq_work(&zone->superpageblocks[j].defrag_irq_work,
					      spb_defrag_irq_work_fn);
			}
		}
	}

	return 0;
}
late_initcall(pageblock_evacuate_init);

/**
 * init_superpageblock_defrag - initialize defrag work structs for a superpageblock
 * @sb: superpageblock to initialize
 *
 * Called during boot from pageblock_evacuate_init() and during memory
 * hotplug from resize_zone_superpageblocks().  Safe to call multiple times
 * on the same superpageblock (reinitializes work structs).
 */
void init_superpageblock_defrag(struct superpageblock *sb)
{
	INIT_WORK(&sb->defrag_work, spb_defrag_work_fn);
	init_irq_work(&sb->defrag_irq_work, spb_defrag_irq_work_fn);
}
#endif /* CONFIG_COMPACTION */

#ifdef CONFIG_CONTIG_ALLOC
/* Usage: See admin-guide/dynamic-debug-howto.rst */
static void alloc_contig_dump_pages(struct list_head *page_list)
{
	DEFINE_DYNAMIC_DEBUG_METADATA(descriptor, "migrate failure");

	if (DYNAMIC_DEBUG_BRANCH(descriptor)) {
		struct page *page;

		dump_stack();
		list_for_each_entry(page, page_list, lru)
			dump_page(page, "migration failure");
	}
}

/* [start, end) must belong to a single zone. */
static int __alloc_contig_migrate_range(struct compact_control *cc,
					unsigned long start, unsigned long end)
{
	/* This function is based on compact_zone() from compaction.c. */
	unsigned int nr_reclaimed;
	unsigned long pfn = start;
	unsigned int tries = 0;
	int ret = 0;
	struct migration_target_control mtc = {
		.nid = zone_to_nid(cc->zone),
		.gfp_mask = cc->gfp_mask,
		.reason = MR_CONTIG_RANGE,
	};

	lru_cache_disable();

	while (pfn < end || !list_empty(&cc->migratepages)) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		if (list_empty(&cc->migratepages)) {
			cc->nr_migratepages = 0;
			ret = isolate_migratepages_range(cc, pfn, end);
			if (ret && ret != -EAGAIN)
				break;
			pfn = cc->migrate_pfn;
			tries = 0;
		} else if (++tries == 5) {
			ret = -EBUSY;
			break;
		}

		nr_reclaimed = reclaim_clean_pages_from_list(cc->zone,
							&cc->migratepages);
		cc->nr_migratepages -= nr_reclaimed;

		ret = migrate_pages(&cc->migratepages, alloc_migration_target,
			NULL, (unsigned long)&mtc, cc->mode, MR_CONTIG_RANGE, NULL);

		/*
		 * On -ENOMEM, migrate_pages() bails out right away. It is pointless
		 * to retry again over this error, so do the same here.
		 */
		if (ret == -ENOMEM)
			break;
	}

	lru_cache_enable();
	if (ret < 0) {
		if (!(cc->gfp_mask & __GFP_NOWARN) && ret == -EBUSY)
			alloc_contig_dump_pages(&cc->migratepages);
		putback_movable_pages(&cc->migratepages);
	}

	return (ret < 0) ? ret : 0;
}

static void split_free_frozen_pages(struct list_head *list, gfp_t gfp_mask)
{
	int order;

	for (order = 0; order < NR_PAGE_ORDERS; order++) {
		struct page *page, *next;
		int nr_pages = 1 << order;

		list_for_each_entry_safe(page, next, &list[order], lru) {
			int i;

			post_alloc_hook(page, order, gfp_mask);
			if (!order)
				continue;

			__split_page(page, order);

			/* Add all subpages to the order-0 head, in sequence. */
			list_del(&page->lru);
			for (i = 0; i < nr_pages; i++)
				list_add_tail(&page[i].lru, &list[0]);
		}
	}
}

static int __alloc_contig_verify_gfp_mask(gfp_t gfp_mask, gfp_t *gfp_cc_mask)
{
	const gfp_t reclaim_mask = __GFP_IO | __GFP_FS | __GFP_RECLAIM;
	const gfp_t action_mask = __GFP_COMP | __GFP_RETRY_MAYFAIL | __GFP_NOWARN |
				  __GFP_ZERO | __GFP_ZEROTAGS | __GFP_SKIP_ZERO |
				  __GFP_SKIP_KASAN;
	const gfp_t cc_action_mask = __GFP_RETRY_MAYFAIL | __GFP_NOWARN;

	/*
	 * We are given the range to allocate; node, mobility and placement
	 * hints are irrelevant at this point. We'll simply ignore them.
	 */
	gfp_mask &= ~(GFP_ZONEMASK | __GFP_RECLAIMABLE | __GFP_WRITE |
		      __GFP_HARDWALL | __GFP_THISNODE | __GFP_MOVABLE);

	/*
	 * We only support most reclaim flags (but not NOFAIL/NORETRY), and
	 * selected action flags.
	 */
	if (gfp_mask & ~(reclaim_mask | action_mask))
		return -EINVAL;

	/*
	 * Flags to control page compaction/migration/reclaim, to free up our
	 * page range. Migratable pages are movable, __GFP_MOVABLE is implied
	 * for them.
	 *
	 * Traditionally we always had __GFP_RETRY_MAYFAIL set, keep doing that
	 * to not degrade callers.
	 */
	*gfp_cc_mask = (gfp_mask & (reclaim_mask | cc_action_mask)) |
			__GFP_MOVABLE | __GFP_RETRY_MAYFAIL;
	return 0;
}

static void __free_contig_frozen_range(unsigned long pfn, unsigned long nr_pages)
{
	for (; nr_pages--; pfn++)
		free_frozen_pages(pfn_to_page(pfn), 0);
}

/**
 * alloc_contig_frozen_range() -- tries to allocate given range of frozen pages
 * @start:	start PFN to allocate
 * @end:	one-past-the-last PFN to allocate
 * @alloc_flags:	allocation information
 * @gfp_mask:	GFP mask. Node/zone/placement hints are ignored; only some
 *		action and reclaim modifiers are supported. Reclaim modifiers
 *		control allocation behavior during compaction/migration/reclaim.
 *
 * The PFN range does not have to be pageblock aligned. The PFN range must
 * belong to a single zone.
 *
 * The first thing this routine does is attempt to MIGRATE_ISOLATE all
 * pageblocks in the range.  Once isolated, the pageblocks should not
 * be modified by others.
 *
 * All frozen pages which PFN is in [start, end) are allocated for the
 * caller, and they could be freed with free_contig_frozen_range(),
 * free_frozen_pages() also could be used to free compound frozen pages
 * directly.
 *
 * Return: zero on success or negative error code.
 */
int alloc_contig_frozen_range_noprof(unsigned long start, unsigned long end,
		acr_flags_t alloc_flags, gfp_t gfp_mask)
{
	const unsigned int order = ilog2(end - start);
	unsigned long outer_start, outer_end;
	int ret = 0;

	struct compact_control cc = {
		.nr_migratepages = 0,
		.order = -1,
		.zone = page_zone(pfn_to_page(start)),
		.mode = MIGRATE_SYNC,
		.ignore_skip_hint = true,
		.no_set_skip_hint = true,
		.alloc_contig = true,
	};
	INIT_LIST_HEAD(&cc.migratepages);
	enum pb_isolate_mode mode = (alloc_flags & ACR_FLAGS_CMA) ?
					    PB_ISOLATE_MODE_CMA_ALLOC :
					    PB_ISOLATE_MODE_OTHER;

	/*
	 * In contrast to the buddy, we allow for orders here that exceed
	 * MAX_PAGE_ORDER, so we must manually make sure that we are not
	 * exceeding the maximum folio order.
	 */
	if (WARN_ON_ONCE((gfp_mask & __GFP_COMP) && order > MAX_FOLIO_ORDER))
		return -EINVAL;

	gfp_mask = current_gfp_context(gfp_mask);
	if (__alloc_contig_verify_gfp_mask(gfp_mask, (gfp_t *)&cc.gfp_mask))
		return -EINVAL;

	/*
	 * What we do here is we mark all pageblocks in range as
	 * MIGRATE_ISOLATE.  Because pageblock and max order pages may
	 * have different sizes, and due to the way page allocator
	 * work, start_isolate_page_range() has special handlings for this.
	 *
	 * Once the pageblocks are marked as MIGRATE_ISOLATE, we
	 * migrate the pages from an unaligned range (ie. pages that
	 * we are interested in). This will put all the pages in
	 * range back to page allocator as MIGRATE_ISOLATE.
	 *
	 * When this is done, we take the pages in range from page
	 * allocator removing them from the buddy system.  This way
	 * page allocator will never consider using them.
	 *
	 * This lets us mark the pageblocks back as
	 * MIGRATE_CMA/MIGRATE_MOVABLE so that free pages in the
	 * aligned range but not in the unaligned, original range are
	 * put back to page allocator so that buddy can use them.
	 */

	ret = start_isolate_page_range(start, end, mode);
	if (ret)
		goto done;

	drain_all_pages(cc.zone);

	/*
	 * In case of -EBUSY, we'd like to know which page causes problem.
	 * So, just fall through. test_pages_isolated() has a tracepoint
	 * which will report the busy page.
	 *
	 * It is possible that busy pages could become available before
	 * the call to test_pages_isolated, and the range will actually be
	 * allocated.  So, if we fall through be sure to clear ret so that
	 * -EBUSY is not accidentally used or returned to caller.
	 */
	ret = __alloc_contig_migrate_range(&cc, start, end);
	if (ret && ret != -EBUSY)
		goto done;

	/*
	 * When in-use hugetlb pages are migrated, they may simply be released
	 * back into the free hugepage pool instead of being returned to the
	 * buddy system.  After the migration of in-use huge pages is completed,
	 * we will invoke replace_free_hugepage_folios() to ensure that these
	 * hugepages are properly released to the buddy system.
	 */
	ret = replace_free_hugepage_folios(start, end);
	if (ret)
		goto done;

	/*
	 * Pages from [start, end) are within a pageblock_nr_pages
	 * aligned blocks that are marked as MIGRATE_ISOLATE.  What's
	 * more, all pages in [start, end) are free in page allocator.
	 * What we are going to do is to allocate all pages from
	 * [start, end) (that is remove them from page allocator).
	 *
	 * The only problem is that pages at the beginning and at the
	 * end of interesting range may be not aligned with pages that
	 * page allocator holds, ie. they can be part of higher order
	 * pages.  Because of this, we reserve the bigger range and
	 * once this is done free the pages we are not interested in.
	 *
	 * We don't have to hold zone->lock here because the pages are
	 * isolated thus they won't get removed from buddy.
	 */
	outer_start = find_large_buddy(start);

	/* Make sure the range is really isolated. */
	if (test_pages_isolated(outer_start, end, mode)) {
		ret = -EBUSY;
		goto done;
	}

	/* Grab isolated pages from freelists. */
	outer_end = isolate_freepages_range(&cc, outer_start, end);
	if (!outer_end) {
		ret = -EBUSY;
		goto done;
	}

	if (!(gfp_mask & __GFP_COMP)) {
		split_free_frozen_pages(cc.freepages, gfp_mask);

		/* Free head and tail (if any) */
		if (start != outer_start)
			__free_contig_frozen_range(outer_start, start - outer_start);
		if (end != outer_end)
			__free_contig_frozen_range(end, outer_end - end);
	} else if (start == outer_start && end == outer_end && is_power_of_2(end - start)) {
		struct page *head = pfn_to_page(start);

		check_new_pages(head, order);
		prep_new_page(head, order, gfp_mask, 0);
	} else {
		ret = -EINVAL;
		WARN(true, "PFN range: requested [%lu, %lu), allocated [%lu, %lu)\n",
		     start, end, outer_start, outer_end);
	}
done:
	undo_isolate_page_range(start, end);
	if (!ret)
		superpageblock_contig_mark(start, end, true);
	return ret;
}
EXPORT_SYMBOL(alloc_contig_frozen_range_noprof);

/**
 * alloc_contig_range() -- tries to allocate given range of pages
 * @start:	start PFN to allocate
 * @end:	one-past-the-last PFN to allocate
 * @alloc_flags:	allocation information
 * @gfp_mask:	GFP mask.
 *
 * This routine is a wrapper around alloc_contig_frozen_range(), it can't
 * be used to allocate compound pages, the refcount of each allocated page
 * will be set to one.
 *
 * All pages which PFN is in [start, end) are allocated for the caller,
 * and should be freed with free_contig_range() or by manually calling
 * __free_page() on each allocated page.
 *
 * Return: zero on success or negative error code.
 */
int alloc_contig_range_noprof(unsigned long start, unsigned long end,
			      acr_flags_t alloc_flags, gfp_t gfp_mask)
{
	int ret;

	if (WARN_ON(gfp_mask & __GFP_COMP))
		return -EINVAL;

	ret = alloc_contig_frozen_range_noprof(start, end, alloc_flags, gfp_mask);
	if (!ret)
		set_pages_refcounted(pfn_to_page(start), end - start);

	return ret;
}
EXPORT_SYMBOL(alloc_contig_range_noprof);

static bool pfn_range_valid_contig(struct zone *z, unsigned long start_pfn,
				   unsigned long nr_pages, bool skip_hugetlb,
				   bool *skipped_hugetlb)
{
	unsigned long end_pfn = start_pfn + nr_pages;
	struct page *page;

	while (start_pfn < end_pfn) {
		unsigned long step = 1;

		page = pfn_to_online_page(start_pfn);
		if (!page)
			return false;

		if (page_zone(page) != z)
			return false;

		if (page_is_unmovable(z, page, PB_ISOLATE_MODE_OTHER, &step))
			return false;

		/*
		 * Only consider ranges containing hugepages if those pages are
		 * smaller than the requested contiguous region.  e.g.:
		 *     Move 2MB pages to free up a 1GB range.
		 *     Don't move 1GB pages to free up a 2MB range.
		 *
		 * This makes contiguous allocation more reliable if multiple
		 * hugepage sizes are used without causing needless movement.
		 */
		if (PageHuge(page)) {
			unsigned int order;

			if (skip_hugetlb) {
				*skipped_hugetlb = true;
				return false;
			}

			page = compound_head(page);
			order = compound_order(page);
			if ((order >= MAX_FOLIO_ORDER) ||
			    (nr_pages <= (1 << order)))
				return false;
		}

		start_pfn += step;
	}
	return true;
}

static bool zone_spans_last_pfn(const struct zone *zone,
				unsigned long start_pfn, unsigned long nr_pages)
{
	unsigned long last_pfn = start_pfn + nr_pages - 1;

	return zone_spans_pfn(zone, last_pfn);
}

/*
 * Maximum superpageblock candidates to collect for contiguous allocation.
 * Collected under zone->lock, then tried without it.
 */
#define SPB_CONTIG_MAX_CANDIDATES 4

/*
 * Maximum tainted superpageblock candidates per spb_evacuate_for_order call.
 * Collected under zone->lock, then evacuated without it. Larger than the
 * contig-allocation candidate cap because evacuation runs from the slowpath
 * after reclaim/compaction failed: we need a meaningful chance of freeing a
 * non-MOV-claimable pageblock before the slowpath escalates to dropping
 * ALLOC_NOFRAGMENT (which lets __rmqueue_claim taint clean SPBs). Sized to
 * scan a meaningful fraction of a typical tainted-pool population.
 */
#define SPB_EVACUATE_MAX_CANDIDATES 16

/*
 * Maximum pageblocks to evacuate per candidate SPB inside
 * spb_evacuate_for_order. Each evacuation triggers page migration which is
 * O(pages_per_pageblock) wall-clock cost, so this caps per-call latency.
 * Bumped from 3 to 8 to free more capacity per slowpath escalation pass.
 * Combined cap: SPB_EVACUATE_MAX_CANDIDATES * SPB_EVACUATE_MAX_PB_PER_SB
 * pageblocks per call (16 * 8 = 128 = 256 MiB on x86 max migration budget).
 */
#define SPB_EVACUATE_MAX_PB_PER_SB 8

#ifdef CONFIG_COMPACTION
/**
 * sb_collect_contig_candidates - Find superpageblock ranges for contiguous alloc
 * @zone: zone to search (must hold zone->lock)
 * @nr_pages: number of contiguous pages needed
 * @pfns: output array of candidate start PFNs
 * @max: maximum candidates to collect
 *
 * For superpageblock-sized (1GB) allocations:
 *   1. Empty superpageblocks first — no evacuation needed
 *   2. Clean superpageblocks from almost-empty to full — less evacuation work
 *
 * For pageblock-sized (2MB+) sub-superpageblock allocations:
 *   1. Clean superpageblocks from fullest to almost-empty — pack allocations
 *      to preserve empty superpageblocks for 1GB
 *   2. Empty superpageblocks as last resort
 *
 * Returns number of candidates found.
 */
static int sb_collect_contig_candidates(struct zone *zone,
					unsigned long nr_pages,
					unsigned long *pfns, int max)
{
	struct superpageblock *sb;
	int full, n = 0;

	lockdep_assert_held(&zone->lock);

	if (nr_pages >= SUPERPAGEBLOCK_NR_PAGES) {
		/* 1GB+: empty superpageblocks first (no evacuation needed) */
		list_for_each_entry(sb, &zone->spb_empty, list) {
			if (sb->total_pageblocks < SUPERPAGEBLOCK_NR_PAGEBLOCKS)
				continue;
			pfns[n++] = sb->start_pfn;
			if (n >= max)
				return n;
		}
		/* Then clean superpageblocks, almost-empty first (less work) */
		for (full = __NR_SB_FULLNESS - 1; full >= 0; full--) {
			list_for_each_entry(sb,
					    &zone->spb_lists[SB_CLEAN][full],
					    list) {
				if (sb->total_pageblocks <
				    SUPERPAGEBLOCK_NR_PAGEBLOCKS)
					continue;
				pfns[n++] = sb->start_pfn;
				if (n >= max)
					return n;
			}
		}
		return n;
	}

	/*
	 * 2MB+ sub-superpageblock allocations.
	 * Walk clean superpageblocks fullest-first — pack allocations into
	 * partial superpageblocks to preserve empty ones for 1GB use.
	 * Pick one candidate per superpageblock for diversity.
	 */
	for (full = SB_FULL_75; full < __NR_SB_FULLNESS; full++) {
		list_for_each_entry(sb, &zone->spb_lists[SB_CLEAN][full], list) {
			unsigned long pfn, sb_end;

			sb_end = sb->start_pfn +
				(unsigned long)sb->total_pageblocks *
				pageblock_nr_pages;
			pfn = ALIGN(sb->start_pfn, nr_pages);

			if (pfn + nr_pages <= sb_end) {
				pfns[n++] = pfn;
				if (n >= max)
					return n;
			}
		}
	}
	/* Empty superpageblocks as last resort for 2MB */
	list_for_each_entry(sb, &zone->spb_empty, list) {
		unsigned long pfn = ALIGN(sb->start_pfn, nr_pages);
		unsigned long sb_end = sb->start_pfn +
			(unsigned long)sb->total_pageblocks *
			pageblock_nr_pages;

		if (pfn + nr_pages <= sb_end) {
			pfns[n++] = pfn;
			if (n >= max)
				return n;
		}
	}
	return n;
}

/**
 * spb_try_alloc_contig - Superpageblock-aware contiguous page allocation
 * @zone: zone to allocate from
 * @nr_pages: number of contiguous pages needed (>= pageblock_nr_pages)
 * @gfp_mask: GFP mask for allocation
 *
 * Use superpageblock metadata to quickly find suitable ranges for contiguous
 * allocation, avoiding the brute-force PFN scan. Each candidate is tried
 * twice to handle transient failures (e.g., temporary page pins, racing
 * allocations), then falls through to the next candidate.
 *
 * Returns: page pointer on success, NULL on failure.
 */
static struct page *spb_try_alloc_contig(struct zone *zone,
					unsigned long nr_pages,
					gfp_t gfp_mask)
{
	unsigned long pfns[SPB_CONTIG_MAX_CANDIDATES];
	unsigned long flags;
	int nr_candidates, i;

	if (nr_pages < pageblock_nr_pages)
		return NULL;

	spin_lock_irqsave(&zone->lock, flags);
	nr_candidates = sb_collect_contig_candidates(zone, nr_pages,
						     pfns,
						     SPB_CONTIG_MAX_CANDIDATES);
	spin_unlock_irqrestore(&zone->lock, flags);

	for (i = 0; i < nr_candidates; i++) {
		int attempts;

		for (attempts = 0; attempts < 2; attempts++) {
			int ret;

			ret = alloc_contig_frozen_range_noprof(pfns[i],
					pfns[i] + nr_pages,
					ACR_FLAGS_NONE, gfp_mask);
			if (!ret)
				return pfn_to_page(pfns[i]);
		}

		/*
		 * Failed on this candidate — rotate its superpageblock to the
		 * tail of its list so the next call tries fresh candidates.
		 */
		spin_lock_irqsave(&zone->lock, flags);
		{
			struct superpageblock *sb =
				pfn_to_superpageblock(zone, pfns[i]);
			if (sb) {
				struct list_head *head;

				if (sb->nr_free == sb->total_pageblocks)
					head = &zone->spb_empty;
				else
					head = &zone->spb_lists
						[spb_get_category(sb)]
						[sb_get_fullness(sb, spb_get_category(sb))];
				list_move_tail(&sb->list, head);
			}
		}
		spin_unlock_irqrestore(&zone->lock, flags);
	}
	return NULL;
}

/**
 * sb_collect_evacuate_candidates - Find tainted SPBs for targeted evacuation
 * @zone: zone to search (must hold zone->lock)
 * @migratetype: desired migratetype (MIGRATE_UNMOVABLE or MIGRATE_RECLAIMABLE),
 *               or -1 to find any tainted SPB with movable pages
 * @sb_pfns: output array of tainted superpageblock start PFNs
 * @max: maximum candidates to collect
 *
 * Find tainted superpageblocks with movable pages to evacuate.  When
 * @migratetype is specified, only return SPBs that also contain pageblocks
 * of that type (for coalescing within existing non-movable pageblocks).
 * When @migratetype is -1, return any tainted SPB with movable pages
 * (for freeing whole pageblocks via movable evacuation).
 *
 * Returns number of candidate superpageblock PFNs found.
 */
static int sb_collect_evacuate_candidates(struct zone *zone,
					  unsigned long *sb_pfns, int max)
{
	struct superpageblock *sb;
	int full, n = 0;

	lockdep_assert_held(&zone->lock);

	for (full = 0; full < __NR_SB_FULLNESS; full++) {
		list_for_each_entry(sb, &zone->spb_lists[SB_TAINTED][full],
				    list) {
			if (!sb->nr_movable)
				continue;

			sb_pfns[n++] = sb->start_pfn;
			if (n >= max)
				return n;
		}
	}
	return n;
}

/*
 * Evacuate MOV content out of any pageblock in the given range that has it.
 *
 * The previous version filtered on the source pageblock's migratetype tag,
 * which made evacuation blind to MOV stragglers living in PBs whose tag did
 * not match the current allocation's requesting type:
 *
 *   - PASS_2C / PASS_2D borrows set PB_has_<requesting_mt> on a MOV-tagged
 *     PB without changing the tag. The borrowed pages return to the MOV
 *     free list when freed, so a MOV-tagged PB can host non-MOV PB_has bits
 *     and MOV content simultaneously.
 *
 *   - When __spb_set_has_type adds a non-MOV bit on a PB, the PB tag is not
 *     re-evaluated. PBs accumulate has-bits over time without their tag
 *     necessarily reflecting current content.
 *
 * Drop the migratetype tag filter and accept any PB with PB_has_movable set.
 * Skip only the cases whose semantics forbid touching them here:
 *   - MIGRATE_ISOLATE     under quarantine
 *   - CMA                 own allocator
 *   - MIGRATE_HIGHATOMIC  reserve, evac would race the reservation logic
 *
 * Returns number of pageblocks evacuated.
 */
static int evacuate_pb_range(struct zone *zone, unsigned long start_pfn,
			     unsigned long end_pfn, int max)
{
	unsigned long pfn;
	int nr_evacuated = 0;

	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		struct page *page;
		int pb_mt;

		if (!pfn_valid(pfn))
			continue;

		if (!zone_spans_pfn(zone, pfn))
			continue;

		page = pfn_to_page(pfn);

		if (!get_pfnblock_bit(page, pfn, PB_has_movable))
			continue;

		pb_mt = get_pfnblock_migratetype(page, pfn);
		if (pb_mt == MIGRATE_ISOLATE ||
		    is_migrate_cma(pb_mt) ||
		    pb_mt == MIGRATE_HIGHATOMIC)
			continue;

		evacuate_pageblock(zone, pfn, true);
		if (++nr_evacuated >= max)
			break;
	}
	return nr_evacuated;
}

/**
 * spb_evacuate_for_order - Targeted evacuation of movable pages from
 *                         tainted superpageblocks
 * @zone: zone to work on
 * @order: allocation order that failed
 * @migratetype: desired migratetype (MIGRATE_UNMOVABLE or MIGRATE_RECLAIMABLE)
 *
 * Two-phase evacuation to create free space in tainted superpageblocks:
 *
 * Phase 1: Evacuate movable pages from pageblocks already labeled as
 * @migratetype. This creates buddy coalescing opportunities within
 * existing non-movable pageblocks.
 *
 * Phase 2: Evacuate entire MOVABLE pageblocks from tainted SPBs.
 * When fully evacuated, these become free whole pageblocks that
 * __rmqueue_smallest Pass 2 can claim for the desired migratetype.
 *
 * Returns true if evacuation was performed (caller should retry allocation).
 */
static bool spb_evacuate_for_order(struct zone *zone, unsigned int order,
				  int migratetype)
{
	unsigned long sb_pfns[SPB_EVACUATE_MAX_CANDIDATES];
	unsigned long flags;
	int nr_sbs, i;
	unsigned int attempts = 0;
	bool did_evacuate = false;

	/*
	 * Single-pass evacuation: collect candidate tainted SPBs (anything
	 * with MOV content), then walk each one's pageblocks evacuating MOV
	 * content from any non-special PB. evacuate_pb_range filters by
	 * PB_has_movable, so this is a no-op on PBs that have no MOV content.
	 *
	 * Two effects accumulate:
	 *   - PBs that are pure MOV become empty -> free MOV pageblock,
	 *     claimable by Pass 2 / claim_whole_block on the retry.
	 *   - PBs that are mixed (e.g., UNMOV + MOV stragglers) lose the MOV
	 *     stragglers, so future allocations of the dominant type can use
	 *     the PB without competing with the MOV residue.
	 *
	 * The previous two-phase design tried to do these separately and
	 * filtered evacuation by source PB tag. That left MOV content
	 * stranded in PBs whose tag did not match either phase, and gave up
	 * after one phase even though the other phase could have helped.
	 */
	spin_lock_irqsave(&zone->lock, flags);
	nr_sbs = sb_collect_evacuate_candidates(zone, sb_pfns,
						SPB_EVACUATE_MAX_CANDIDATES);
	spin_unlock_irqrestore(&zone->lock, flags);

	for (i = 0; i < nr_sbs; i++) {
		unsigned long end_pfn = sb_pfns[i] + SUPERPAGEBLOCK_NR_PAGES;
		int n;

		n = evacuate_pb_range(zone, sb_pfns[i], end_pfn,
				      SPB_EVACUATE_MAX_PB_PER_SB);
		attempts += n;
		if (n)
			did_evacuate = true;
	}

	/*
	 * Always kick a slab shrink after an evacuation pass — even when
	 * MOV evacuation succeeded. Slab content stranded inside tainted
	 * SPBs can only be freed by shrinking the cache; doing it now keeps
	 * headroom available for the next burst, when the MOV supply may
	 * have run out and evac alone would have nothing to do.
	 */
	queue_spb_slab_shrink(zone);

	/*
	 * The tracepoint signature retains phase1_attempts / phase2_attempts
	 * for ABI continuity with existing observers; report the merged total
	 * in phase1_attempts and 0 in phase2_attempts.
	 */
	trace_spb_evacuate_for_order_done(zone, order, migratetype,
			attempts, 0, did_evacuate);
	return did_evacuate;
}
#endif /* CONFIG_COMPACTION */

/**
 * alloc_contig_frozen_pages() -- tries to find and allocate contiguous range of frozen pages
 * @nr_pages:	Number of contiguous pages to allocate
 * @gfp_mask:	GFP mask. Node/zone/placement hints limit the search; only some
 *		action and reclaim modifiers are supported. Reclaim modifiers
 *		control allocation behavior during compaction/migration/reclaim.
 * @nid:	Target node
 * @nodemask:	Mask for other possible nodes
 *
 * This routine is a wrapper around alloc_contig_frozen_range(). It scans over
 * zones on an applicable zonelist to find a contiguous pfn range which can then
 * be tried for allocation with alloc_contig_frozen_range(). This routine is
 * intended for allocation requests which can not be fulfilled with the buddy
 * allocator.
 *
 * The allocated memory is always aligned to a page boundary. If nr_pages is a
 * power of two, then allocated range is also guaranteed to be aligned to same
 * nr_pages (e.g. 1GB request would be aligned to 1GB).
 *
 * Allocated frozen pages need be freed with free_contig_frozen_range(),
 * or by manually calling free_frozen_pages() on each allocated frozen
 * non-compound page, for compound frozen pages could be freed with
 * free_frozen_pages() directly.
 *
 * Return: pointer to contiguous frozen pages on success, or NULL if not successful.
 */
struct page *alloc_contig_frozen_pages_noprof(unsigned long nr_pages,
		gfp_t gfp_mask, int nid, nodemask_t *nodemask)
{
	unsigned long ret, pfn, flags;
	struct zonelist *zonelist;
	struct zone *zone;
	struct zoneref *z;
	struct page *page;
	bool skip_hugetlb = true;
	bool skipped_hugetlb = false;

	/*
	 * First pass: superpageblock-aware search. Use superpageblock metadata
	 * to quickly find suitable ranges, avoiding the brute-force PFN
	 * scan. For 1GB allocations this walks spb_empty then
	 * spb_lists[SB_CLEAN]; for 2MB+ it finds evacuatable pageblocks
	 * in clean superpageblocks.
	 */
	if (nr_pages >= pageblock_nr_pages) {
		zonelist = node_zonelist(nid, gfp_mask);
		for_each_zone_zonelist_nodemask(zone, z, zonelist,
					       gfp_zone(gfp_mask), nodemask) {
			page = spb_try_alloc_contig(zone, nr_pages, gfp_mask);
			if (page)
				return page;
		}
	}

	/* Second pass: brute-force PFN scan (existing fallback) */

retry:
	zonelist = node_zonelist(nid, gfp_mask);
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(gfp_mask), nodemask) {
		spin_lock_irqsave(&zone->lock, flags);

		pfn = ALIGN(zone->zone_start_pfn, nr_pages);
		while (zone_spans_last_pfn(zone, pfn, nr_pages)) {
			if (pfn_range_valid_contig(zone, pfn, nr_pages,
						   skip_hugetlb,
						   &skipped_hugetlb)) {
				/*
				 * We release the zone lock here because
				 * alloc_contig_frozen_range() will also lock
				 * the zone at some point. If there's an
				 * allocation spinning on this lock, it may
				 * win the race and cause allocation to fail.
				 */
				spin_unlock_irqrestore(&zone->lock, flags);
				ret = alloc_contig_frozen_range_noprof(pfn,
							pfn + nr_pages,
							ACR_FLAGS_NONE,
							gfp_mask);
				if (!ret)
					return pfn_to_page(pfn);
				spin_lock_irqsave(&zone->lock, flags);
			}
			pfn += nr_pages;
		}
		spin_unlock_irqrestore(&zone->lock, flags);
	}
	/*
	 * If we failed, retry the search, but treat regions with HugeTLB pages
	 * as valid targets.  This retains fast-allocations on first pass
	 * without trying to migrate HugeTLB pages (which may fail). On the
	 * second pass, we will try moving HugeTLB pages when those pages are
	 * smaller than the requested contiguous region size.
	 */
	if (skip_hugetlb && skipped_hugetlb) {
		skip_hugetlb = false;
		goto retry;
	}
	return NULL;
}
EXPORT_SYMBOL(alloc_contig_frozen_pages_noprof);

/**
 * alloc_contig_pages() -- tries to find and allocate contiguous range of pages
 * @nr_pages:	Number of contiguous pages to allocate
 * @gfp_mask:	GFP mask.
 * @nid:	Target node
 * @nodemask:	Mask for other possible nodes
 *
 * This routine is a wrapper around alloc_contig_frozen_pages(), it can't
 * be used to allocate compound pages, the refcount of each allocated page
 * will be set to one.
 *
 * Allocated pages can be freed with free_contig_range() or by manually
 * calling __free_page() on each allocated page.
 *
 * Return: pointer to contiguous pages on success, or NULL if not successful.
 */
struct page *alloc_contig_pages_noprof(unsigned long nr_pages, gfp_t gfp_mask,
		int nid, nodemask_t *nodemask)
{
	struct page *page;

	if (WARN_ON(gfp_mask & __GFP_COMP))
		return NULL;

	page = alloc_contig_frozen_pages_noprof(nr_pages, gfp_mask, nid,
						nodemask);
	if (page)
		set_pages_refcounted(page, nr_pages);

	return page;
}
EXPORT_SYMBOL(alloc_contig_pages_noprof);

/**
 * free_contig_frozen_range() -- free the contiguous range of frozen pages
 * @pfn:	start PFN to free
 * @nr_pages:	Number of contiguous frozen pages to free
 *
 * This can be used to free the allocated compound/non-compound frozen pages.
 */
void free_contig_frozen_range(unsigned long pfn, unsigned long nr_pages)
{
	struct page *first_page = pfn_to_page(pfn);
	const unsigned int order = ilog2(nr_pages);

	if (WARN_ON_ONCE(first_page != compound_head(first_page)))
		return;

	superpageblock_contig_mark(pfn, pfn + nr_pages, false);

	if (PageHead(first_page)) {
		WARN_ON_ONCE(order != compound_order(first_page));
		free_frozen_pages(first_page, order);
		return;
	}

	__free_contig_frozen_range(pfn, nr_pages);
}
EXPORT_SYMBOL(free_contig_frozen_range);

/**
 * free_contig_range() -- free the contiguous range of pages
 * @pfn:	start PFN to free
 * @nr_pages:	Number of contiguous pages to free
 *
 * This can be only used to free the allocated non-compound pages.
 */
void free_contig_range(unsigned long pfn, unsigned long nr_pages)
{
	unsigned long end = pfn + nr_pages;

	if (WARN_ON_ONCE(PageHead(pfn_to_page(pfn))))
		return;

	superpageblock_contig_mark(pfn, end, false);

	for (; nr_pages--; pfn++)
		__free_page(pfn_to_page(pfn));
}
EXPORT_SYMBOL(free_contig_range);
#endif /* CONFIG_CONTIG_ALLOC */

/*
 * Effectively disable pcplists for the zone by setting the high limit to 0
 * and draining all cpus. A concurrent page freeing on another CPU that's about
 * to put the page on pcplist will either finish before the drain and the page
 * will be drained, or observe the new high limit and skip the pcplist.
 *
 * Must be paired with a call to zone_pcp_enable().
 */
void zone_pcp_disable(struct zone *zone)
{
	mutex_lock(&pcp_batch_high_lock);
	__zone_set_pageset_high_and_batch(zone, 0, 0, 1);
	__drain_all_pages(zone, true);
}

void zone_pcp_enable(struct zone *zone)
{
	__zone_set_pageset_high_and_batch(zone, zone->pageset_high_min,
		zone->pageset_high_max, zone->pageset_batch);
	mutex_unlock(&pcp_batch_high_lock);
}

void zone_pcp_reset(struct zone *zone)
{
	int cpu;
	struct per_cpu_zonestat *pzstats;

	if (zone->per_cpu_pageset != &boot_pageset) {
		for_each_online_cpu(cpu) {
			pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
			drain_zonestat(zone, pzstats);
		}
		free_percpu(zone->per_cpu_pageset);
		zone->per_cpu_pageset = &boot_pageset;
		if (zone->per_cpu_zonestats != &boot_zonestats) {
			free_percpu(zone->per_cpu_zonestats);
			zone->per_cpu_zonestats = &boot_zonestats;
		}
	}
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * All pages in the range must be in a single zone, must not contain holes,
 * must span full sections, and must be isolated before calling this function.
 *
 * Returns the number of managed (non-PageOffline()) pages in the range: the
 * number of pages for which memory offlining code must adjust managed page
 * counters using adjust_managed_page_count().
 */
unsigned long __offline_isolated_pages(unsigned long start_pfn,
		unsigned long end_pfn)
{
	unsigned long already_offline = 0, flags;
	unsigned long pfn = start_pfn;
	struct page *page;
	struct zone *zone;
	unsigned int order;

	offline_mem_sections(pfn, end_pfn);
	zone = page_zone(pfn_to_page(pfn));
	spin_lock_irqsave(&zone->lock, flags);
	while (pfn < end_pfn) {
		page = pfn_to_page(pfn);
		/*
		 * The HWPoisoned page may be not in buddy system, and
		 * page_count() is not 0.
		 */
		if (unlikely(!PageBuddy(page) && PageHWPoison(page))) {
			pfn++;
			continue;
		}
		/*
		 * At this point all remaining PageOffline() pages have a
		 * reference count of 0 and can simply be skipped.
		 */
		if (PageOffline(page)) {
			BUG_ON(page_count(page));
			BUG_ON(PageBuddy(page));
			already_offline++;
			pfn++;
			continue;
		}

		BUG_ON(page_count(page));
		BUG_ON(!PageBuddy(page));
		VM_WARN_ON(get_pageblock_migratetype(page) != MIGRATE_ISOLATE);
		order = buddy_order(page);
		if (order >= pageblock_order)
			superpageblock_range_now_used(page, order);
		del_page_from_free_list(page, zone, order, MIGRATE_ISOLATE);
		pfn += (1 << order);
	}
	spin_unlock_irqrestore(&zone->lock, flags);

	return end_pfn - start_pfn - already_offline;
}
#endif

/*
 * This function returns a stable result only if called under zone lock.
 */
bool is_free_buddy_page(const struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned int order;

	for (order = 0; order < NR_PAGE_ORDERS; order++) {
		const struct page *head = page - (pfn & ((1 << order) - 1));

		if (PageBuddy(head) &&
		    buddy_order_unsafe(head) >= order)
			break;
	}

	return order <= MAX_PAGE_ORDER;
}
EXPORT_SYMBOL(is_free_buddy_page);

#ifdef CONFIG_MEMORY_FAILURE
static inline void add_to_free_list(struct page *page, struct zone *zone,
				    unsigned int order, int migratetype,
				    bool tail)
{
	__add_to_free_list(page, zone, order, migratetype, tail);
	account_freepages(zone, 1 << order, migratetype);
}

/*
 * Break down a higher-order page in sub-pages, and keep our target out of
 * buddy allocator.
 */
static void break_down_buddy_pages(struct zone *zone, struct page *page,
				   struct page *target, int low, int high,
				   int migratetype)
{
	unsigned long size = 1 << high;
	struct page *current_buddy;

	while (high > low) {
		high--;
		size >>= 1;

		if (target >= &page[size]) {
			current_buddy = page;
			page = page + size;
		} else {
			current_buddy = page + size;
		}

		if (set_page_guard(zone, current_buddy, high))
			continue;

		add_to_free_list(current_buddy, zone, high, migratetype, false);
		set_buddy_order(current_buddy, high);
	}
}

/*
 * Take a page that will be marked as poisoned off the buddy allocator.
 */
bool take_page_off_buddy(struct page *page)
{
	struct zone *zone = page_zone(page);
	unsigned long pfn = page_to_pfn(page);
	unsigned long flags;
	unsigned int order;
	bool ret = false;

	spin_lock_irqsave(&zone->lock, flags);
	for (order = 0; order < NR_PAGE_ORDERS; order++) {
		struct page *page_head = page - (pfn & ((1 << order) - 1));
		int page_order = buddy_order(page_head);

		if (PageBuddy(page_head) && page_order >= order) {
			unsigned long pfn_head = page_to_pfn(page_head);
			int migratetype = get_pfnblock_migratetype(page_head,
								   pfn_head);

			del_page_from_free_list(page_head, zone, page_order,
						migratetype);
			/*
			 * break_down_buddy_pages() re-adds every non-target
			 * pageblock to buddy at order >= pageblock_order, so
			 * those keep their PB_all_free state. Only the target's
			 * pageblock loses its fully-free status — clear that
			 * one bit and decrement the SPB nr_free counter.
			 */
			if (page_order >= pageblock_order) {
				unsigned long pfn_pb = ALIGN_DOWN(pfn,
							pageblock_nr_pages);
				struct page *pb_page = pfn_to_page(pfn_pb);

				if (get_pfnblock_bit(pb_page, pfn_pb,
						     PB_all_free)) {
					clear_pfnblock_bit(pb_page, pfn_pb,
							   PB_all_free);
					superpageblock_pb_now_used(pb_page);
				}
			}
			break_down_buddy_pages(zone, page_head, page, 0,
						page_order, migratetype);
			SetPageHWPoisonTakenOff(page);
			ret = true;
			break;
		}
		if (page_count(page_head) > 0)
			break;
	}
	spin_unlock_irqrestore(&zone->lock, flags);
	return ret;
}

/*
 * Cancel takeoff done by take_page_off_buddy().
 */
bool put_page_back_buddy(struct page *page)
{
	struct zone *zone = page_zone(page);
	unsigned long flags;
	bool ret = false;

	spin_lock_irqsave(&zone->lock, flags);
	if (put_page_testzero(page)) {
		unsigned long pfn = page_to_pfn(page);
		int migratetype = get_pfnblock_migratetype(page, pfn);

		ClearPageHWPoisonTakenOff(page);
		__free_one_page(page, pfn, zone, 0, migratetype, FPI_NONE);
		if (TestClearPageHWPoison(page)) {
			ret = true;
		}
	}
	spin_unlock_irqrestore(&zone->lock, flags);

	return ret;
}
#endif

bool has_managed_zone(enum zone_type zone)
{
	struct pglist_data *pgdat;

	for_each_online_pgdat(pgdat) {
		if (managed_zone(&pgdat->node_zones[zone]))
			return true;
	}
	return false;
}

#ifdef CONFIG_UNACCEPTED_MEMORY

static bool lazy_accept = true;

static int __init accept_memory_parse(char *p)
{
	if (!strcmp(p, "lazy")) {
		lazy_accept = true;
		return 0;
	} else if (!strcmp(p, "eager")) {
		lazy_accept = false;
		return 0;
	} else {
		return -EINVAL;
	}
}
early_param("accept_memory", accept_memory_parse);

static bool page_contains_unaccepted(struct page *page, unsigned int order)
{
	phys_addr_t start = page_to_phys(page);

	return range_contains_unaccepted_memory(start, PAGE_SIZE << order);
}

static void __accept_page(struct zone *zone, unsigned long *flags,
			  struct page *page)
{
	list_del(&page->lru);
	account_freepages(zone, -MAX_ORDER_NR_PAGES, MIGRATE_MOVABLE);
	__mod_zone_page_state(zone, NR_UNACCEPTED, -MAX_ORDER_NR_PAGES);
	__ClearPageUnaccepted(page);
	spin_unlock_irqrestore(&zone->lock, *flags);

	accept_memory(page_to_phys(page), PAGE_SIZE << MAX_PAGE_ORDER);

	__free_pages_ok(page, MAX_PAGE_ORDER, FPI_TO_TAIL);
}

void accept_page(struct page *page)
{
	struct zone *zone = page_zone(page);
	unsigned long flags;

	spin_lock_irqsave(&zone->lock, flags);
	if (!PageUnaccepted(page)) {
		spin_unlock_irqrestore(&zone->lock, flags);
		return;
	}

	/* Unlocks zone->lock */
	__accept_page(zone, &flags, page);
}

static bool try_to_accept_memory_one(struct zone *zone)
{
	unsigned long flags;
	struct page *page;

	spin_lock_irqsave(&zone->lock, flags);
	page = list_first_entry_or_null(&zone->unaccepted_pages,
					struct page, lru);
	if (!page) {
		spin_unlock_irqrestore(&zone->lock, flags);
		return false;
	}

	/* Unlocks zone->lock */
	__accept_page(zone, &flags, page);

	return true;
}

static bool cond_accept_memory(struct zone *zone, unsigned int order,
			       int alloc_flags)
{
	long to_accept, wmark;
	bool ret = false;

	if (list_empty(&zone->unaccepted_pages))
		return false;

	/* Bailout, since try_to_accept_memory_one() needs to take a lock */
	if (alloc_flags & ALLOC_TRYLOCK)
		return false;

	wmark = promo_wmark_pages(zone);

	/*
	 * Watermarks have not been initialized yet.
	 *
	 * Accepting one MAX_ORDER page to ensure progress.
	 */
	if (!wmark)
		return try_to_accept_memory_one(zone);

	/* How much to accept to get to promo watermark? */
	to_accept = wmark -
		    (zone_page_state(zone, NR_FREE_PAGES) -
		    __zone_watermark_unusable_free(zone, order, 0) -
		    zone_page_state(zone, NR_UNACCEPTED));

	while (to_accept > 0) {
		if (!try_to_accept_memory_one(zone))
			break;
		ret = true;
		to_accept -= MAX_ORDER_NR_PAGES;
	}

	return ret;
}

static bool __free_unaccepted(struct page *page)
{
	struct zone *zone = page_zone(page);
	unsigned long flags;

	if (!lazy_accept)
		return false;

	spin_lock_irqsave(&zone->lock, flags);
	list_add_tail(&page->lru, &zone->unaccepted_pages);
	account_freepages(zone, MAX_ORDER_NR_PAGES, MIGRATE_MOVABLE);
	__mod_zone_page_state(zone, NR_UNACCEPTED, MAX_ORDER_NR_PAGES);
	__SetPageUnaccepted(page);
	spin_unlock_irqrestore(&zone->lock, flags);

	return true;
}

#else

static bool page_contains_unaccepted(struct page *page, unsigned int order)
{
	return false;
}

static bool cond_accept_memory(struct zone *zone, unsigned int order,
			       int alloc_flags)
{
	return false;
}

static bool __free_unaccepted(struct page *page)
{
	BUILD_BUG();
	return false;
}

#endif /* CONFIG_UNACCEPTED_MEMORY */

struct page *alloc_frozen_pages_nolock_noprof(gfp_t gfp_flags, int nid, unsigned int order)
{
	/*
	 * Do not specify __GFP_DIRECT_RECLAIM, since direct claim is not allowed.
	 * Do not specify __GFP_KSWAPD_RECLAIM either, since wake up of kswapd
	 * is not safe in arbitrary context.
	 *
	 * These two are the conditions for gfpflags_allow_spinning() being true.
	 *
	 * Specify __GFP_NOWARN since failing alloc_pages_nolock() is not a reason
	 * to warn. Also warn would trigger printk() which is unsafe from
	 * various contexts. We cannot use printk_deferred_enter() to mitigate,
	 * since the running context is unknown.
	 *
	 * Specify __GFP_ZERO to make sure that call to kmsan_alloc_page() below
	 * is safe in any context. Also zeroing the page is mandatory for
	 * BPF use cases.
	 *
	 * Though __GFP_NOMEMALLOC is not checked in the code path below,
	 * specify it here to highlight that alloc_pages_nolock()
	 * doesn't want to deplete reserves.
	 */
	gfp_t alloc_gfp = __GFP_NOWARN | __GFP_ZERO | __GFP_NOMEMALLOC | __GFP_COMP
			| gfp_flags;
	unsigned int alloc_flags = ALLOC_TRYLOCK;
	struct alloc_context ac = { };
	struct page *page;

	VM_WARN_ON_ONCE(gfp_flags & ~__GFP_ACCOUNT);
	/*
	 * In PREEMPT_RT spin_trylock() will call raw_spin_lock() which is
	 * unsafe in NMI. If spin_trylock() is called from hard IRQ the current
	 * task may be waiting for one rt_spin_lock, but rt_spin_trylock() will
	 * mark the task as the owner of another rt_spin_lock which will
	 * confuse PI logic, so return immediately if called from hard IRQ or
	 * NMI.
	 *
	 * Note, irqs_disabled() case is ok. This function can be called
	 * from raw_spin_lock_irqsave region.
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && (in_nmi() || in_hardirq()))
		return NULL;
	if (!pcp_allowed_order(order))
		return NULL;

	/* Bailout, since _deferred_grow_zone() needs to take a lock */
	if (deferred_pages_enabled())
		return NULL;

	if (nid == NUMA_NO_NODE)
		nid = numa_node_id();

	prepare_alloc_pages(alloc_gfp, order, nid, NULL, &ac,
			    &alloc_gfp, &alloc_flags);

	/*
	 * Best effort allocation from percpu free list.
	 * If it's empty attempt to spin_trylock zone->lock.
	 */
	page = get_page_from_freelist(alloc_gfp, order, alloc_flags, &ac);

	/* Unlike regular alloc_pages() there is no __alloc_pages_slowpath(). */

	if (memcg_kmem_online() && page && (gfp_flags & __GFP_ACCOUNT) &&
	    unlikely(__memcg_kmem_charge_page(page, alloc_gfp, order) != 0)) {
		__free_frozen_pages(page, order, FPI_TRYLOCK);
		page = NULL;
	}
	trace_mm_page_alloc(page, order, alloc_gfp, ac.migratetype);
	kmsan_alloc_page(page, order, alloc_gfp);
	return page;
}
/**
 * alloc_pages_nolock - opportunistic reentrant allocation from any context
 * @gfp_flags: GFP flags. Only __GFP_ACCOUNT allowed.
 * @nid: node to allocate from
 * @order: allocation order size
 *
 * Allocates pages of a given order from the given node. This is safe to
 * call from any context (from atomic, NMI, and also reentrant
 * allocator -> tracepoint -> alloc_pages_nolock_noprof).
 * Allocation is best effort and to be expected to fail easily so nobody should
 * rely on the success. Failures are not reported via warn_alloc().
 * See always fail conditions below.
 *
 * Return: allocated page or NULL on failure. NULL does not mean EBUSY or EAGAIN.
 * It means ENOMEM. There is no reason to call it again and expect !NULL.
 */
struct page *alloc_pages_nolock_noprof(gfp_t gfp_flags, int nid, unsigned int order)
{
	struct page *page;

	page = alloc_frozen_pages_nolock_noprof(gfp_flags, nid, order);
	if (page)
		set_page_refcounted(page);
	return page;
}
EXPORT_SYMBOL_GPL(alloc_pages_nolock_noprof);

#ifdef CONFIG_DEBUG_FS
static const char * const sb_fullness_names[] = {
	"full", "75pct", "50pct", "25pct", "almost_empty"
};

static const char * const sb_category_names[] = {
	"clean", "tainted"
};

static int superpageblock_debugfs_show(struct seq_file *m, void *v)
{
	struct zone *zone;
	int cat, full;

	for_each_populated_zone(zone) {
		unsigned long i;
		int empty_count = 0;
		struct superpageblock *sb;

		if (!zone->superpageblocks)
			continue;

		seq_printf(m, "Node %d, zone %8s: %lu superpageblocks, base_pfn=0x%lx\n",
			   zone->zone_pgdat->node_id, zone->name,
			   zone->nr_superpageblocks, zone->superpageblock_base_pfn);

		list_for_each_entry(sb, &zone->spb_empty, list)
			empty_count++;
		if (empty_count)
			seq_printf(m, "  empty: %d\n", empty_count);

		{
			int isolated_count = 0;

			list_for_each_entry(sb, &zone->spb_isolated, list)
				isolated_count++;
			if (isolated_count)
				seq_printf(m, "  contig_alloc: %d\n", isolated_count);
		}

		for (cat = 0; cat < __NR_SB_CATEGORIES; cat++) {
			for (full = 0; full < __NR_SB_FULLNESS; full++) {
				int count = 0;

				list_for_each_entry(sb,
					&zone->spb_lists[cat][full], list)
					count++;
				if (count)
					seq_printf(m, "  %s/%s: %d\n",
						   sb_category_names[cat],
						   sb_fullness_names[full],
						   count);
			}
		}

		/* Per-superpageblock detail */
		for (i = 0; i < zone->nr_superpageblocks; i++) {
			sb = &zone->superpageblocks[i];
			if (sb->contig_allocated)
				seq_printf(m, "  sb[%lu] pfn=0x%lx: contig_allocated total=%u\n",
					   i, sb->start_pfn,
					   sb->total_pageblocks);
			else
				seq_printf(m, "  sb[%lu] pfn=0x%lx: unmov=%u recl=%u mov=%u rsv=%u free=%u total=%u free_pages=%lu\n",
					   i, sb->start_pfn,
					   sb->nr_unmovable, sb->nr_reclaimable,
					   sb->nr_movable, sb->nr_reserved,
					   sb->nr_free, sb->total_pageblocks,
					   sb->nr_free_pages);
		}
	}
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(superpageblock_debugfs);

static int __init superpageblock_debugfs_init(void)
{
	debugfs_create_file("superpageblocks", 0444, NULL, NULL,
			    &superpageblock_debugfs_fops);
	return 0;
}
late_initcall(superpageblock_debugfs_init);
#endif /* CONFIG_DEBUG_FS */
