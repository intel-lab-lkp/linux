// SPDX-License-Identifier: GPL-2.0
/*
 * Workingset detection
 *
 * Copyright (C) 2013 Red Hat, Inc., Johannes Weiner
 */

#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/writeback.h>
#include <linux/shmem_fs.h>
#include <linux/pagemap.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include "swap_table.h"
#include "internal.h"

/*
 *		Double CLOCK lists
 *
 * Per node, two clock lists are maintained for file pages: the
 * inactive and the active list.  Freshly faulted pages start out at
 * the head of the inactive list and page reclaim scans pages from the
 * tail.  Pages that are accessed multiple times on the inactive list
 * are promoted to the active list, to protect them from reclaim,
 * whereas active pages are demoted to the inactive list when the
 * active list grows too big.
 *
 *   fault ------------------------+
 *                                 |
 *              +--------------+   |            +-------------+
 *   reclaim <- |   inactive   | <-+-- demotion |    active   | <--+
 *              +--------------+                +-------------+    |
 *                     |                                           |
 *                     +-------------- promotion ------------------+
 *
 *
 *		Access frequency and refault distance
 *
 * A workload is thrashing when its pages are frequently used but they
 * are evicted from the inactive list every time before another access
 * would have promoted them to the active list.
 *
 * In cases where the average access distance between thrashing pages
 * is bigger than the size of memory there is nothing that can be
 * done - the thrashing set could never fit into memory under any
 * circumstance.
 *
 * However, the average access distance could be bigger than the
 * inactive list, yet smaller than the size of memory.  In this case,
 * the set could fit into memory if it weren't for the currently
 * active pages - which may be used more, hopefully less frequently:
 *
 *      +-memory available to cache-+
 *      |                           |
 *      +-inactive------+-active----+
 *  a b | c d e f g h i | J K L M N |
 *      +---------------+-----------+
 *
 * It is prohibitively expensive to accurately track access frequency
 * of pages.  But a reasonable approximation can be made to measure
 * thrashing on the inactive list, after which refaulting pages can be
 * activated optimistically to compete with the existing active pages.
 *
 * For such approximation, introduce a counter `evictions` (E)
 * per lruvec. E is incremented once for every evicted page, and each
 * evicted page's shadow entry records the E value at eviction time as
 * a timestamp. So when an evicted page is faulted in again, we have:
 *
 *   Let SP = ((E's reading @ current) - (E's reading @ eviction))
 *
 *                            +-memory available to cache-+
 *                            |                           |
 *  +-------------------------+===============+===========+
 *  |       shadows    *      |   INACTIVE    |   ACTIVE  |
 *  +-+------------^----------+===============+===========+
 *    |            |
 *    +------------+
 *    |     SP
 *  oldest shadow       * -> The refaulting page's shadow in the
 *                           imaginary "Shadow LRU"
 *
 * SP stands for how far back in eviction history the refaulting page
 * is. Since every evicted page was once at the head of the INACTIVE
 * list, the minimum in-memory access distance this page would have
 * needed to avoid eviction is:
 *
 *   SP + NR_INACTIVE
 *
 * So the page is a plausible workingset candidate if:
 *
 *   SP + NR_INACTIVE <= NR_INACTIVE + NR_ACTIVE
 *
 * which simplifies to:
 *
 *   SP <= NR_ACTIVE
 *
 * Note NR_INACTIVE is read at refault time, not at eviction time; the
 * two can differ, but the difference is assumed small for workloads
 * stable enough that the refault-distance heuristic is meaningful at
 * all.
 *
 * The derivation above gives the upper bound on when activation could
 * keep the page resident. The actual policy used here is stricter:
 * the refault distance is compared against the average of NR_ACTIVE
 * and NR_INACTIVE rather than NR_ACTIVE alone:
 *
 *   SP <= (NR_ACTIVE + NR_INACTIVE) / 2
 *
 * Two arguments motivate this threshold and converge on the same
 * operating point:
 *
 * 1. Self-balancing around the feasibility bound. Relative to
 *    SP <= NR_ACTIVE:
 *
 *    - when NR_ACTIVE is short (no established workingset), the
 *      threshold sits above NR_ACTIVE, allowing more activations so
 *      a workingset can form faster;
 *    - when NR_ACTIVE is long (established workingset), the threshold
 *      sits below NR_ACTIVE, so one-time refaults do not easily
 *      displace established active pages.
 *
 *    Because NR_ACTIVE + NR_INACTIVE is roughly M (total cache
 *    memory), the threshold is roughly M/2 regardless of how the A/I
 *    split moves.
 *
 * 2. Bounded MRU-like protection. For a sequential cyclic scan over
 *    a set of size S on memory M, every refault has SP roughly equal
 *    to S - M in steady state. With the threshold at M/2, activation
 *    happens exactly when S <= 1.5 * M. In that regime the heuristic
 *    freezes roughly M/2 pages onto the active list, yielding a large
 *    hit-rate improvement over pure LRU (which has ~0% hits on any
 *    cyclic scan with S > M). Above 1.5 * M the heuristic disengages:
 *    partial protection has diminishing returns as S/M grows, and
 *    indiscriminate activation would pollute the active list with
 *    pages unlikely to be re-accessed before eviction. For random
 *    access with skew, the SP-based threshold naturally selects
 *    hotter pages (shorter inter-access times give smaller SP), so
 *    the benefit extends across a broader range of S/M as a smooth
 *    transition rather than a cliff.
 *
 *		Refaulting inactive pages
 *
 * All that is known about the active list is that the pages have been
 * accessed more than once in the past.  This means that at any given
 * time there is actually a good chance that pages on the active list
 * are no longer in active use.
 *
 * So when a refault distance SP satisfies the rule above, the
 * refaulting page is activated optimistically in the hope that roughly
 * (NR_ACTIVE + NR_INACTIVE) / 2 pages on the active side are used less
 * frequently than the refaulting page - or even not used at all anymore.
 *
 * If this is wrong and demotion kicks in, the pages which are truly
 * used more frequently will be reactivated while the less frequently
 * used ones will be evicted from memory.
 *
 * But if this is right, the stale pages will be pushed out of memory
 * and the used pages get to stay in cache.
 *
 *		Refaulting active pages
 *
 * If on the other hand the refaulting pages have recently been
 * deactivated, it means that the active list is no longer protecting
 * actively used cache from reclaim. The cache is NOT transitioning to
 * a different workingset; the existing workingset is thrashing in the
 * space allocated to the page cache.
 *
 *
 *		Implementation
 *
 * For each lruvec, a non-resident age counter (lruvec->evictions)
 * is maintained. It is incremented once per evicted page.
 *
 * On eviction, a snapshot of this counter (along with some bits to
 * identify the lruvec) is stored in the now empty page cache
 * slot of the evicted page.  This is called a shadow entry.
 *
 * On cache misses for which there are shadow entries, an eligible
 * refault distance will immediately activate the refaulting page.
 */

#define WORKINGSET_SHIFT 1
#define EVICTION_SHIFT	((BITS_PER_LONG - BITS_PER_XA_VALUE) +	\
			 WORKINGSET_SHIFT + NODES_SHIFT + \
			 MEM_CGROUP_ID_SHIFT)
#define EVICTION_SHIFT_ANON	(EVICTION_SHIFT + SWAP_COUNT_SHIFT)
#define EVICTION_MASK	(~0UL >> EVICTION_SHIFT)
#define EVICTION_MASK_ANON	(~0UL >> EVICTION_SHIFT_ANON)

/*
 * LRU refs uses LRU_REFS_WIDTH + 2 bits, the 2 bits are PG_workingset and
 * PG_referenced. But here we record PG_workingset separately (to reuse
 * pack_shadow).
 */
#define LRU_REFS_BITS ((LRU_REFS_WIDTH + 2) - 1)

/*
 * Eviction timestamps need to be able to cover the full range of
 * actionable refaults. However, bits are tight in the xarray
 * entry, and after storing the identifier for the lruvec there might
 * not be enough left to represent every single actionable refault. In
 * that case, we have to sacrifice granularity for distance, and group
 * evictions into coarser buckets by shaving off lower timestamp bits.
 */
static unsigned int bucket_order[ANON_AND_FILE] __read_mostly;

static void *pack_shadow(int memcgid, pg_data_t *pgdat, unsigned long eviction,
			 bool workingset, bool file)
{
	eviction &= file ? EVICTION_MASK : EVICTION_MASK_ANON;
	eviction = (eviction << MEM_CGROUP_ID_SHIFT) | memcgid;
	eviction = (eviction << NODES_SHIFT) | pgdat->node_id;
	eviction = (eviction << WORKINGSET_SHIFT) | workingset;

	return xa_mk_value(eviction);
}

static void unpack_shadow(void *shadow, int *memcgidp, pg_data_t **pgdat,
			  unsigned long *evictionp, bool *workingsetp)
{
	unsigned long entry = xa_to_value(shadow);
	int memcgid, nid;
	bool workingset;

	workingset = entry & ((1UL << WORKINGSET_SHIFT) - 1);
	entry >>= WORKINGSET_SHIFT;
	nid = entry & ((1UL << NODES_SHIFT) - 1);
	entry >>= NODES_SHIFT;
	memcgid = entry & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);
	entry >>= MEM_CGROUP_ID_SHIFT;

	*memcgidp = memcgid;
	*pgdat = NODE_DATA(nid);
	*evictionp = entry;
	*workingsetp = workingset;
}

#ifdef CONFIG_LRU_GEN

static void *lru_gen_eviction(struct folio *folio)
{
	int hist;
	unsigned long token;
	unsigned long min_seq;
	struct lruvec *lruvec;
	struct lru_gen_folio *lrugen;
	int type = folio_is_file_lru(folio);
	int delta = folio_nr_pages(folio);
	int refs = folio_lru_refs(folio);
	int tier = lru_tier_from_refs(refs);
	struct mem_cgroup *memcg;
	struct pglist_data *pgdat = folio_pgdat(folio);
	unsigned short memcg_id;

	BUILD_BUG_ON(LRU_GEN_WIDTH + LRU_REFS_BITS >
		     BITS_PER_LONG - max(EVICTION_SHIFT, EVICTION_SHIFT_ANON));

	rcu_read_lock();
	memcg = folio_memcg(folio);
	lruvec = mem_cgroup_lruvec(memcg, pgdat);
	lrugen = &lruvec->lrugen;
	min_seq = READ_ONCE(lrugen->min_seq[type]);
	token = (min_seq << LRU_REFS_BITS) | refs >> 1;

	hist = lru_hist_from_seq(min_seq);
	atomic_long_add(delta, &lrugen->evicted[hist][type][tier]);
	memcg_id = mem_cgroup_private_id(memcg);
	rcu_read_unlock();

	return pack_shadow(memcg_id, pgdat, token, refs & 1, type);
}

/*
 * Tests if the shadow entry is for a folio that was recently evicted.
 * Fills in @lruvec, @token, @workingset with the values unpacked from shadow.
 */
static bool lru_gen_test_recent(void *shadow, struct lruvec **lruvec,
				unsigned long *token, bool *workingset, bool file)
{
	int memcg_id;
	unsigned long max_seq;
	struct mem_cgroup *memcg;
	struct pglist_data *pgdat;

	unpack_shadow(shadow, &memcg_id, &pgdat, token, workingset);

	memcg = mem_cgroup_from_private_id(memcg_id);
	*lruvec = mem_cgroup_lruvec(memcg, pgdat);

	max_seq = READ_ONCE((*lruvec)->lrugen.max_seq);
	max_seq &= (file ? EVICTION_MASK : EVICTION_MASK_ANON) >> LRU_REFS_BITS;

	return abs_diff(max_seq, *token >> LRU_REFS_BITS) < MAX_NR_GENS;
}

static void lru_gen_refault(struct folio *folio, void *shadow)
{
	bool recent;
	int hist, tier, refs;
	bool workingset;
	unsigned long token;
	struct lruvec *lruvec;
	struct lru_gen_folio *lrugen;
	int type = folio_is_file_lru(folio);
	int delta = folio_nr_pages(folio);

	rcu_read_lock();

	recent = lru_gen_test_recent(shadow, &lruvec, &token, &workingset, type);
	if (lruvec != folio_lruvec(folio))
		goto unlock;

	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + type, delta);

	if (!recent)
		goto unlock;

	lrugen = &lruvec->lrugen;

	hist = lru_hist_from_seq(READ_ONCE(lrugen->min_seq[type]));
	refs = ((token & (BIT(LRU_REFS_BITS) - 1)) << 1) + workingset;
	tier = lru_tier_from_refs(refs);

	atomic_long_add(delta, &lrugen->refaulted[hist][type][tier]);

	/* see folio_add_lru() where folio_set_active() will be called */
	if (lru_gen_in_fault())
		mod_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + type, delta);

	if (refs) {
		folio_set_lru_refs(folio, refs);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + type, delta);
	}
unlock:
	rcu_read_unlock();
}

#else /* !CONFIG_LRU_GEN */

static void *lru_gen_eviction(struct folio *folio)
{
	return NULL;
}

static bool lru_gen_test_recent(void *shadow, struct lruvec **lruvec,
				unsigned long *token, bool *workingset, bool file)
{
	return false;
}

static void lru_gen_refault(struct folio *folio, void *shadow)
{
}

#endif /* CONFIG_LRU_GEN */

/**
 * workingset_age_nonresident - age non-resident entries as LRU ages
 * @lruvec: the lruvec that was aged
 * @nr_pages: the number of pages to count
 *
 * As in-memory pages are aged, non-resident pages need to be aged as
 * well, in order for the refault distances later on to be comparable
 * to the in-memory dimensions. This function allows reclaim and LRU
 * operations to drive the non-resident aging along in parallel.
 */
static void workingset_age_nonresident(struct lruvec *lruvec, unsigned long nr_pages)
{
	/*
	 * Reclaiming a cgroup means reclaiming all its children in a
	 * round-robin fashion. That means that each cgroup has an LRU
	 * order that is composed of the LRU orders of its child
	 * cgroups; and every page has an LRU position not just in the
	 * cgroup that owns it, but in all of that group's ancestors.
	 *
	 * So when the physical inactive list of a leaf cgroup ages,
	 * the virtual inactive lists of all its parents, including
	 * the root cgroup's, age as well.
	 */
	do {
		atomic_long_add(nr_pages, &lruvec->evictions);
	} while ((lruvec = parent_lruvec(lruvec)));
}

/**
 * workingset_eviction - note the eviction of a folio from memory
 * @target_memcg: the cgroup that is causing the reclaim
 * @folio: the folio being evicted
 *
 * Return: a shadow entry to be stored in @folio->mapping->i_pages in place
 * of the evicted @folio so that a later refault can be detected.
 */
void *workingset_eviction(struct folio *folio, struct mem_cgroup *target_memcg)
{
	struct pglist_data *pgdat = folio_pgdat(folio);
	int file = folio_is_file_lru(folio);
	unsigned long eviction;
	struct lruvec *lruvec;
	int memcgid;

	/* Folio is fully exclusive and pins folio's memory cgroup pointer */
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);
	VM_BUG_ON_FOLIO(folio_ref_count(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (lru_gen_enabled())
		return lru_gen_eviction(folio);

	lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	/* XXX: target_memcg can be NULL, go through lruvec */
	memcgid = mem_cgroup_private_id(lruvec_memcg(lruvec));
	eviction = atomic_long_read(&lruvec->evictions);
	eviction >>= bucket_order[file];
	workingset_age_nonresident(lruvec, folio_nr_pages(folio));
	return pack_shadow(memcgid, pgdat, eviction,
			   folio_is_workingset(folio), file);
}

/**
 * workingset_test_recent - tests if the shadow entry is for a folio that was
 * recently evicted. Also fills in @workingset with the value unpacked from
 * shadow.
 * @shadow: the shadow entry to be tested.
 * @file: whether the corresponding folio is from the file lru.
 * @workingset: where the workingset value unpacked from shadow should
 * be stored.
 * @flush: whether to flush cgroup rstat.
 *
 * Return: true if the shadow is for a recently evicted folio; false otherwise.
 */
bool workingset_test_recent(void *shadow, bool file, bool *workingset,
				bool flush)
{
	unsigned long refault, distance, active, inactive;
	struct mem_cgroup *eviction_memcg;
	struct lruvec *eviction_lruvec;
	struct pglist_data *pgdat;
	unsigned long eviction;
	int memcgid;

	if (lru_gen_enabled()) {
		bool recent;

		rcu_read_lock();
		recent = lru_gen_test_recent(shadow, &eviction_lruvec, &eviction,
					     workingset, file);
		rcu_read_unlock();
		return recent;
	}

	rcu_read_lock();
	unpack_shadow(shadow, &memcgid, &pgdat, &eviction, workingset);
	eviction <<= bucket_order[file];

	/*
	 * Look up the memcg associated with the stored ID. It might
	 * have been deleted since the folio's eviction.
	 *
	 * Note that in rare events the ID could have been recycled
	 * for a new cgroup that refaults a shared folio. This is
	 * impossible to tell from the available data. However, this
	 * should be a rare and limited disturbance, and activations
	 * are always speculative anyway. Ultimately, it's the aging
	 * algorithm's job to shake out the minimum access frequency
	 * for the active cache.
	 *
	 * XXX: On !CONFIG_MEMCG, this will always return NULL; it
	 * would be better if the root_mem_cgroup existed in all
	 * configurations instead.
	 */
	eviction_memcg = mem_cgroup_from_private_id(memcgid);
	if (!mem_cgroup_tryget(eviction_memcg))
		eviction_memcg = NULL;
	rcu_read_unlock();

	if (!mem_cgroup_disabled() && !eviction_memcg)
		return false;
	/*
	 * Flush stats (and potentially sleep) outside the RCU read section.
	 *
	 * Note that workingset_test_recent() itself might be called in RCU read
	 * section (for e.g, in cachestat) - these callers need to skip flushing
	 * stats (via the flush argument).
	 *
	 * XXX: With per-memcg flushing and thresholding, is ratelimiting
	 * still needed here?
	 */
	if (flush)
		mem_cgroup_flush_stats_ratelimited(eviction_memcg);

	eviction_lruvec = mem_cgroup_lruvec(eviction_memcg, pgdat);
	refault = atomic_long_read(&eviction_lruvec->evictions);

	/*
	 * Calculate the refault distance
	 *
	 * The unsigned subtraction here gives an accurate distance
	 * across evictions overflows in most cases. There is a
	 * special case: usually, shadow entries have a short lifetime
	 * and are either refaulted or reclaimed along with the inode
	 * before they get too old.  But it is not impossible for the
	 * evictions to lap a shadow entry in the field, which
	 * can then result in a false small refault distance, leading
	 * to a false activation should this old entry actually
	 * refault again.  However, earlier kernels used to deactivate
	 * unconditionally with *every* reclaim invocation for the
	 * longest time, so the occasional inappropriate activation
	 * leading to pressure on the active list is not a problem.
	 */
	distance = ((refault - eviction) &
		    (file ? EVICTION_MASK : EVICTION_MASK_ANON));

	/*
	 * Compare the distance to the existing workingset size. We
	 * don't activate pages that couldn't stay resident even if
	 * all the memory was available to the workingset. Whether
	 * workingset competition needs to consider anon or not depends
	 * on having free swap space.
	 */
	active = lruvec_page_state(eviction_lruvec, NR_ACTIVE_FILE);
	inactive = lruvec_page_state(eviction_lruvec, NR_INACTIVE_FILE);

	if (mem_cgroup_get_nr_swap_pages(eviction_memcg) > 0) {
		active += lruvec_page_state(eviction_lruvec, NR_ACTIVE_ANON);
		inactive += lruvec_page_state(eviction_lruvec, NR_INACTIVE_ANON);
	}

	mem_cgroup_put(eviction_memcg);

	/*
	 * Be cautious about challenging the existing active working set;
	 * sacrificing the inactive part of the opposite type should be safe.
	 */
	return distance <= (active + inactive) / 2;
}

/**
 * workingset_refault - Evaluate the refault of a previously evicted folio.
 * @folio: The freshly allocated replacement folio.
 * @shadow: Shadow entry of the evicted folio.
 *
 * Calculates and evaluates the refault distance of the previously
 * evicted folio in the context of the node and the memcg whose memory
 * pressure caused the eviction.
 */
void workingset_refault(struct folio *folio, void *shadow)
{
	bool file = folio_is_file_lru(folio);
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;
	bool workingset;
	long nr;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (lru_gen_enabled()) {
		lru_gen_refault(folio, shadow);
		return;
	}

	/*
	 * The activation decision for this folio is made at the level
	 * where the eviction occurred, as that is where the LRU order
	 * during folio reclaim is being determined.
	 *
	 * However, the cgroup that will own the folio is the one that
	 * is actually experiencing the refault event. Make sure the folio is
	 * locked to guarantee folio_memcg() stability throughout.
	 */
	nr = folio_nr_pages(folio);
	memcg = get_mem_cgroup_from_folio(folio);
	lruvec = mem_cgroup_lruvec(memcg, folio_pgdat(folio));
	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + file, nr);

	if (!workingset_test_recent(shadow, file, &workingset, true))
		goto out;

	folio_set_active(folio);
	mod_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + file, nr);

	/* Folio was active prior to eviction */
	if (workingset) {
		folio_mark_workingset_by_bit(folio);
		/*
		 * XXX: Move to folio_add_lru() when it supports new vs
		 * putback
		 */
		lru_note_cost_refault(folio);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + file, nr);
	}
out:
	mem_cgroup_put(memcg);
}

/*
 * Shadow entries reflect the share of the working set that does not
 * fit into memory, so their number depends on the access pattern of
 * the workload.  In most cases, they will refault or get reclaimed
 * along with the inode, but a (malicious) workload that streams
 * through files with a total size several times that of available
 * memory, while preventing the inodes from being reclaimed, can
 * create excessive amounts of shadow nodes.  To keep a lid on this,
 * track shadow nodes and reclaim them when they grow way past the
 * point where they would still be useful.
 */

struct list_lru shadow_nodes;

void workingset_update_node(struct xa_node *node)
{
	struct page *page = virt_to_page(node);

	/*
	 * Track non-empty nodes that contain only shadow entries;
	 * unlink those that contain pages or are being freed.
	 *
	 * Avoid acquiring the list_lru lock when the nodes are
	 * already where they should be. The list_empty() test is safe
	 * as node->private_list is protected by the i_pages lock.
	 */
	lockdep_assert_held(&node->array->xa_lock);

	if (node->count && node->count == node->nr_values) {
		if (list_empty(&node->private_list)) {
			list_lru_add_obj(&shadow_nodes, &node->private_list);
			__inc_node_page_state(page, WORKINGSET_NODES);
		}
	} else {
		if (!list_empty(&node->private_list)) {
			list_lru_del_obj(&shadow_nodes, &node->private_list);
			__dec_node_page_state(page, WORKINGSET_NODES);
		}
	}
}

static unsigned long count_shadow_nodes(struct shrinker *shrinker,
					struct shrink_control *sc)
{
	unsigned long max_nodes;
	unsigned long nodes;
	unsigned long pages;

	nodes = list_lru_shrink_count(&shadow_nodes, sc);
	if (!nodes)
		return SHRINK_EMPTY;

	/*
	 * Approximate a reasonable limit for the nodes
	 * containing shadow entries. We don't need to keep more
	 * shadow entries than possible pages on the active list,
	 * since refault distances bigger than that are dismissed.
	 *
	 * The size of the active list converges toward 100% of
	 * overall page cache as memory grows, with only a tiny
	 * inactive list. Assume the total cache size for that.
	 *
	 * Nodes might be sparsely populated, with only one shadow
	 * entry in the extreme case. Obviously, we cannot keep one
	 * node for every eligible shadow entry, so compromise on a
	 * worst-case density of 1/8th. Below that, not all eligible
	 * refaults can be detected anymore.
	 *
	 * On 64-bit with 7 xa_nodes per page and 64 slots
	 * each, this will reclaim shadow entries when they consume
	 * ~1.8% of available memory:
	 *
	 * PAGE_SIZE / xa_nodes / node_entries * 8 / PAGE_SIZE
	 */
#ifdef CONFIG_MEMCG
	if (sc->memcg) {
		struct lruvec *lruvec;
		int i;

		mem_cgroup_flush_stats_ratelimited(sc->memcg);
		lruvec = mem_cgroup_lruvec(sc->memcg, NODE_DATA(sc->nid));

		for (pages = 0, i = 0; i < NR_LRU_LISTS; i++)
			pages += lruvec_lru_size(lruvec, i, MAX_NR_ZONES - 1);

		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_RECLAIMABLE_B) >> PAGE_SHIFT;
		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_UNRECLAIMABLE_B) >> PAGE_SHIFT;
	} else
#endif
		pages = node_present_pages(sc->nid);

	max_nodes = pages >> (XA_CHUNK_SHIFT - 3);

	if (nodes <= max_nodes)
		return 0;
	return nodes - max_nodes;
}

static enum lru_status shadow_lru_isolate(struct list_head *item,
					  struct list_lru_one *lru,
					  void *arg) __must_hold(lru->lock)
{
	struct xa_node *node = container_of(item, struct xa_node, private_list);
	struct address_space *mapping;
	int ret;

	/*
	 * Page cache insertions and deletions synchronously maintain
	 * the shadow node LRU under the i_pages lock and the
	 * &lru->lock. Because the page cache tree is emptied before
	 * the inode can be destroyed, holding the &lru->lock pins any
	 * address_space that has nodes on the LRU.
	 *
	 * We can then safely transition to the i_pages lock to
	 * pin only the address_space of the particular node we want
	 * to reclaim, take the node off-LRU, and drop the &lru->lock.
	 */

	mapping = container_of(node->array, struct address_space, i_pages);

	/* Coming from the list, invert the lock order */
	if (!xa_trylock(&mapping->i_pages)) {
		spin_unlock_irq(&lru->lock);
		ret = LRU_RETRY;
		goto out;
	}

	/* For page cache we need to hold i_lock */
	if (mapping->host != NULL) {
		if (!spin_trylock(&mapping->host->i_lock)) {
			xa_unlock(&mapping->i_pages);
			spin_unlock_irq(&lru->lock);
			ret = LRU_RETRY;
			goto out;
		}
	}

	list_lru_isolate(lru, item);
	__dec_node_page_state(virt_to_page(node), WORKINGSET_NODES);

	spin_unlock(&lru->lock);

	/*
	 * The nodes should only contain one or more shadow entries,
	 * no pages, so we expect to be able to remove them all and
	 * delete and free the empty node afterwards.
	 */
	if (WARN_ON_ONCE(!node->nr_values))
		goto out_invalid;
	if (WARN_ON_ONCE(node->count != node->nr_values))
		goto out_invalid;
	xa_delete_node(node, workingset_update_node);
	mod_lruvec_kmem_state(node, WORKINGSET_NODERECLAIM, 1);

out_invalid:
	xa_unlock_irq(&mapping->i_pages);
	if (mapping->host != NULL) {
		if (mapping_shrinkable(mapping))
			inode_lru_list_add(mapping->host);
		spin_unlock(&mapping->host->i_lock);
	}
	ret = LRU_REMOVED_RETRY;
out:
	cond_resched();
	return ret;
}

static unsigned long scan_shadow_nodes(struct shrinker *shrinker,
				       struct shrink_control *sc)
{
	/* list_lru lock nests inside the IRQ-safe i_pages lock */
	return list_lru_shrink_walk_irq(&shadow_nodes, sc, shadow_lru_isolate,
					NULL);
}

/*
 * Our list_lru->lock is IRQ-safe as it nests inside the IRQ-safe
 * i_pages lock.
 */
static struct lock_class_key shadow_nodes_key;

static int __init workingset_init(void)
{
	unsigned int timestamp_bits, timestamp_bits_anon;
	struct shrinker *workingset_shadow_shrinker;
	unsigned int max_order;
	int ret = -ENOMEM;

	BUILD_BUG_ON(BITS_PER_LONG < EVICTION_SHIFT);
	/*
	 * Calculate the eviction bucket size to cover the longest
	 * actionable refault distance, which is currently half of
	 * memory (totalram_pages/2). However, memory hotplug may add
	 * some more pages at runtime, so keep working with up to
	 * double the initial memory by using totalram_pages as-is.
	 */
	timestamp_bits = BITS_PER_LONG - EVICTION_SHIFT;
	timestamp_bits_anon = BITS_PER_LONG - EVICTION_SHIFT_ANON;
	max_order = fls_long(totalram_pages() - 1);
	if (max_order > (BITS_PER_LONG - EVICTION_SHIFT))
		bucket_order[WORKINGSET_FILE] = max_order - timestamp_bits;
	if (max_order > timestamp_bits_anon)
		bucket_order[WORKINGSET_ANON] = max_order - timestamp_bits_anon;
	pr_info("workingset: timestamp_bits=%d (anon: %d) max_order=%d bucket_order=%u (anon: %d)\n",
		timestamp_bits, timestamp_bits_anon, max_order,
		bucket_order[WORKINGSET_FILE], bucket_order[WORKINGSET_ANON]);

	workingset_shadow_shrinker = shrinker_alloc(SHRINKER_NUMA_AWARE |
						    SHRINKER_MEMCG_AWARE,
						    "mm-shadow");
	if (!workingset_shadow_shrinker)
		goto err;

	ret = list_lru_init_memcg_key(&shadow_nodes, workingset_shadow_shrinker,
				      &shadow_nodes_key);
	if (ret)
		goto err_list_lru;

	workingset_shadow_shrinker->count_objects = count_shadow_nodes;
	workingset_shadow_shrinker->scan_objects = scan_shadow_nodes;
	/* ->count reports only fully expendable nodes */
	workingset_shadow_shrinker->seeks = 0;

	shrinker_register(workingset_shadow_shrinker);
	return 0;
err_list_lru:
	shrinker_free(workingset_shadow_shrinker);
err:
	return ret;
}
module_init(workingset_init);
