// SPDX-License-Identifier: GPL-2.0
/*
 * Dual-bitmap page allocation consistency checker
 *
 * Provides corruption detection for page allocations using complementary
 * bitmaps. The invariant (primary == ~secondary) detects any single-bit
 * corruption in either bitmap.
 *
 * Based on NVIDIA safety research.
 */

#define pr_fmt(fmt) "page_consistency: " fmt

#include <linux/page_consistency.h>
#include <linux/dual_bitmap.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/bitmap.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/limits.h>

DEFINE_STATIC_KEY_FALSE(page_consistency_enabled);

struct page_consistency_stats {
	atomic64_t pages_tracked;
	atomic64_t alloc_count;
	atomic64_t free_count;
	atomic64_t violations_detected;
};

static struct page_consistency_stats page_consistency_stats;

/* Internal state */
static struct {
	struct dual_bitmap db;
	unsigned long min_pfn;
	unsigned long max_pfn;
} pc_state __ro_after_init;

/**
 * pfn_to_bit - Convert PFN to bitmap bit index
 * @pfn: Page frame number
 *
 * Returns the bit index in the bitmap for the given PFN.
 */
static inline unsigned long pfn_to_bit(unsigned long pfn)
{
	return pfn - pc_state.min_pfn;
}

/**
 * pfn_in_range - Check if PFN is within tracked range
 * @pfn: Page frame number to check
 *
 * Returns true if the PFN is within the range being tracked.
 */
static inline bool pfn_in_range(unsigned long pfn)
{
	return pfn >= pc_state.min_pfn && pfn < pc_state.max_pfn;
}

/**
 * mark_page_state - Update both bitmaps for a page state change
 * @pfn: Page frame number
 * @is_alloc: true for allocation, false for free
 *
 * Updates both bitmaps atomically and detects double-alloc/double-free.
 * Double-free detection is deferred until system_state reaches SYSTEM_RUNNING
 * because reserved boot memory pages may be freed via free_reserved_area()
 * and free_initmem() without ever being allocated through the buddy allocator.
 *
 * Returns true if the primary bit actually transitioned to the requested
 * state (0->1 for alloc, 1->0 for free), false if it was already in that
 * state. Callers use this to keep pages_tracked an accurate reflection of
 * the number of bits set in the primary bitmap.
 */
static bool mark_page_state(unsigned long pfn, bool is_alloc)
{
	unsigned long bit = pfn_to_bit(pfn);
	bool was_allocated;

	/*
	 * Check the complement invariant before the update. The dual bitops
	 * below unconditionally write the secondary bit, so a corruption
	 * confined to the secondary bitmap would be silently erased by the
	 * very next alloc/free on that PFN. Primary-only corruption is still
	 * caught via the was_allocated check; this pre-check closes the gap
	 * for the secondary side so that corruption is reported symmetrically.
	 */
	if (unlikely(!dual_bitmap_consistent(&pc_state.db, bit))) {
		atomic64_inc(&page_consistency_stats.violations_detected);
#ifdef CONFIG_DEBUG_PAGE_CONSISTENCY_PANIC
		panic("page_consistency: bitmap corruption at PFN %lu before %s\n",
		      pfn, is_alloc ? "alloc" : "free");
#else
		WARN(1, "page_consistency: bitmap corruption at PFN %lu before %s\n",
		     pfn, is_alloc ? "alloc" : "free");
#endif
	}

	if (is_alloc) {
		was_allocated = dual_bitmap_set(&pc_state.db, bit);
		if (unlikely(was_allocated)) {
			atomic64_inc(&page_consistency_stats.violations_detected);
#ifdef CONFIG_DEBUG_PAGE_CONSISTENCY_PANIC
			panic("page_consistency: DOUBLE-ALLOC detected: PFN %lu\n",
			      pfn);
#else
			WARN(1, "page_consistency: DOUBLE-ALLOC detected: PFN %lu\n",
			     pfn);
#endif
			return false;
		}
		return true;
	}

	was_allocated = dual_bitmap_clear(&pc_state.db, bit);
	if (!was_allocated) {
		/*
		 * Only flag double-free after system is fully running.
		 * During boot, free_reserved_area() and free_initmem() free
		 * pages never allocated through the buddy allocator - these
		 * are not bugs. system_state reaches SYSTEM_RUNNING only after
		 * all such freeing is complete.
		 */
		if (unlikely(system_state >= SYSTEM_RUNNING)) {
			atomic64_inc(&page_consistency_stats.violations_detected);
#ifdef CONFIG_DEBUG_PAGE_CONSISTENCY_PANIC
			panic("page_consistency: DOUBLE-FREE detected: PFN %lu\n",
			      pfn);
#else
			WARN(1, "page_consistency: DOUBLE-FREE detected: PFN %lu\n",
			     pfn);
#endif
		}
		return false;
	}
	return true;
}

/**
 * __page_consistency_alloc - Track page allocation
 * @page: Allocated page
 * @order: Allocation order
 *
 * Called from post_alloc_hook() when page_consistency_enabled is true.
 */
void __page_consistency_alloc(struct page *page, unsigned int order)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned int nr_pages = 1U << order;
	unsigned long last_pfn = pfn + nr_pages - 1;
	unsigned int i, transitions = 0;

	if (!pfn_in_range(pfn) || !pfn_in_range(last_pfn))
		return;

	for (i = 0; i < nr_pages; i++)
		if (mark_page_state(pfn + i, true))
			transitions++;

	atomic64_add(transitions, &page_consistency_stats.pages_tracked);
	atomic64_inc(&page_consistency_stats.alloc_count);
}

/**
 * __page_consistency_free - Track page free
 * @page: Page being freed
 * @order: Free order
 *
 * Called from free_pages_prepare() when page_consistency_enabled is true.
 */
void __page_consistency_free(struct page *page, unsigned int order)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned int nr_pages = 1U << order;
	unsigned long last_pfn = pfn + nr_pages - 1;
	unsigned int i, transitions = 0;

	if (!pfn_in_range(pfn) || !pfn_in_range(last_pfn))
		return;

	for (i = 0; i < nr_pages; i++)
		if (mark_page_state(pfn + i, false))
			transitions++;

	atomic64_sub(transitions, &page_consistency_stats.pages_tracked);
	atomic64_inc(&page_consistency_stats.free_count);
}

/**
 * page_consistency_check_page - Check consistency for a single page
 * @page: Page to check
 *
 * Returns PAGE_CONSISTENCY_OK if consistent, PAGE_CONSISTENCY_MISMATCH
 * if corruption detected, or PAGE_CONSISTENCY_NOT_TRACKED if outside range.
 */
enum page_consistency_result page_consistency_check_page(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long bit;

	if (!pfn_in_range(pfn))
		return PAGE_CONSISTENCY_NOT_TRACKED;

	bit = pfn_to_bit(pfn);

	if (!dual_bitmap_consistent(&pc_state.db, bit)) {
		atomic64_inc(&page_consistency_stats.violations_detected);
		pr_err("Consistency violation for PFN %lu\n", pfn);
		return PAGE_CONSISTENCY_MISMATCH;
	}

	return PAGE_CONSISTENCY_OK;
}

/**
 * page_consistency_validate_all - Validate entire bitmap
 *
 * Performs a full consistency check of all bitmap words.
 * Returns PAGE_CONSISTENCY_OK if all consistent, PAGE_CONSISTENCY_MISMATCH
 * if any violations found.
 */
enum page_consistency_result page_consistency_validate_all(void)
{
	unsigned long violations;

	violations = dual_bitmap_validate(&pc_state.db);

	if (violations) {
		/*
		 * violations counts inconsistent words, not bits. One word
		 * could contain up to BITS_PER_LONG corrupted bits.
		 */
		atomic64_add(violations, &page_consistency_stats.violations_detected);
		pr_err("Validation found %lu inconsistent words\n", violations);
		return PAGE_CONSISTENCY_MISMATCH;
	}

	pr_info("Validation passed: %u bits checked\n", pc_state.db.nbits);
	return PAGE_CONSISTENCY_OK;
}

#ifdef CONFIG_DEBUG_FS
/* Debugfs interface */

static int stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "pages_tracked:       %lld\n",
		   atomic64_read(&page_consistency_stats.pages_tracked));
	seq_printf(m, "alloc_count:         %lld\n",
		   atomic64_read(&page_consistency_stats.alloc_count));
	seq_printf(m, "free_count:          %lld\n",
		   atomic64_read(&page_consistency_stats.free_count));
	seq_printf(m, "violations_detected: %lld\n",
		   atomic64_read(&page_consistency_stats.violations_detected));
	seq_printf(m, "bitmap_size_bits:    %u\n", pc_state.db.nbits);
	seq_printf(m, "pfn_range:           [%lu-%lu)\n",
		   pc_state.min_pfn, pc_state.max_pfn);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static ssize_t validate_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	int result = page_consistency_validate_all();

	return result == PAGE_CONSISTENCY_OK ? count : -EIO;
}

static const struct file_operations validate_fops = {
	.write = validate_write,
	.llseek = noop_llseek,
};

static int __init page_consistency_debugfs_init(void)
{
	struct dentry *dir;

	if (!static_key_enabled(&page_consistency_enabled.key))
		return 0;

	dir = debugfs_create_dir("page_consistency", NULL);
	debugfs_create_file("stats", 0444, dir, NULL, &stats_fops);
	debugfs_create_file("validate", 0200, dir, NULL, &validate_fops);

	return 0;
}
late_initcall(page_consistency_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

/**
 * page_consistency_init - Initialize the page consistency checker
 *
 * Called during mm initialization to set up the dual bitmap tracking.
 * Must be called while memblock is still active (before memblock_free_all()).
 */
void __init page_consistency_init(void)
{
	unsigned long spanned_pfns;
	size_t bitmap_bytes;

	/*
	 * Size bitmaps to cover the full PFN range including any holes.
	 * Holes waste a few bits but a flat bitmap keeps the indexing
	 * trivial (pfn - min_pfn) and avoids additional data structures
	 * that would themselves be subject to corruption.  This matches
	 * the approach used by pageblock_flags.
	 */
	pc_state.min_pfn = PHYS_PFN(memblock_start_of_DRAM());
	pc_state.max_pfn = PHYS_PFN(memblock_end_of_DRAM());
	spanned_pfns = pc_state.max_pfn - pc_state.min_pfn;
	if (!spanned_pfns || spanned_pfns > UINT_MAX) {
		pr_err("PFN span %lu cannot be represented by bitmap APIs, feature disabled\n",
		       spanned_pfns);
		return;
	}

	pc_state.db.nbits = spanned_pfns;

	bitmap_bytes = BITS_TO_LONGS(pc_state.db.nbits) * sizeof(unsigned long);

	pr_info("Initializing: PFN range [%lu-%lu), %u bits (%zu KB per bitmap)\n",
		pc_state.min_pfn, pc_state.max_pfn, pc_state.db.nbits,
		bitmap_bytes / 1024);

	/* Allocate primary bitmap (zeroed by memblock_alloc) */
	pc_state.db.bitmap[DUAL_BITMAP_PRIMARY] =
		memblock_alloc(bitmap_bytes, SMP_CACHE_BYTES);
	if (!pc_state.db.bitmap[DUAL_BITMAP_PRIMARY]) {
		pr_err("Failed to allocate primary bitmap, feature disabled\n");
		return;
	}

	/* Allocate secondary bitmap */
	pc_state.db.bitmap[DUAL_BITMAP_SECONDARY] =
		memblock_alloc(bitmap_bytes, SMP_CACHE_BYTES);
	if (!pc_state.db.bitmap[DUAL_BITMAP_SECONDARY]) {
		pr_err("Failed to allocate secondary bitmap, feature disabled\n");
		memblock_free(pc_state.db.bitmap[DUAL_BITMAP_PRIMARY],
			      bitmap_bytes);
		pc_state.db.bitmap[DUAL_BITMAP_PRIMARY] = NULL;
		return;
	}

	/*
	 * Initialize: primary all zeros (already done by memblock_alloc),
	 * secondary all ones. Use dual_bitmap_init() for consistency.
	 */
	dual_bitmap_init(&pc_state.db);

	/* Enable tracking */
	static_branch_enable(&page_consistency_enabled);
	pr_info("Initialized successfully, tracking enabled\n");
}
