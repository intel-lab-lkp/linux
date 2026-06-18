// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON SPE (Statistical Profiling Extension) feedback
 *
 * Provides sub-THP access heatmap for intelligent split decisions.
 *
 * Architecture:
 *   Phase 2a (current): PTE access bits via folio_walk.
 *     Works only when a THP has been previously split to PTEs.
 *     Returns -EOPNOTSUPP for PMD-mapped THPs.
 *
 *   Phase 2b (userspace): spe_hist daemon decodes SPE in userspace,
 *     feeds {pfn, subpage_idx} via debugfs/sysfs into the rbtree below.
 *
 *   Phase 2c (kernel): perf_event_create_kernel_counter for ARM SPE,
 *     overflow handler aggregates into rbtree.  Requires SPE hardware.
 *
 * Data structure:
 *   Per-folio rbtree keyed by PFN, storing per-subpage access counts.
 *   Entries are aged and pruned periodically.
 *
 * Copyright (C) 2026 Wang Lian <lianux.mm@gmail.com>
 */

#define pr_fmt(fmt) "damon-spe: " fmt

#include <linux/mm.h>
#include <linux/pagewalk.h>
#include <linux/huge_mm.h>
#include <linux/bitmap.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/init.h>
#include "spe.h"

/* Max sub-pages when querying at order 0 */
#define DAMON_SPE_MAX_CHUNKS	512

/* Max folio entries in the rbtree (per-mm or global) */
#define DAMON_SPE_MAX_ENTRIES	1024

/* Entry considered stale after this many jiffies (default: 30s) */
#define DAMON_SPE_ENTRY_TTL	(30 * HZ)

/*
 * Per-folio access histogram entry.
 * Keyed by pfn in an rbtree.  Each entry tracks access count per subpage.
 * The access_count array is sized for PMD-order / 0 = 512 4KB subpages.
 */
struct damon_spe_entry {
	struct rb_node		node;
	unsigned long		pfn;		/* THP-aligned PFN */
	pid_t			pid;		/* owner process */
	unsigned long		access_count[DAMON_SPE_MAX_CHUNKS];
	unsigned long		total_accesses;
	unsigned long		last_access;	/* jiffies of last update */
};

static struct rb_root spe_tree = RB_ROOT;
static DEFINE_SPINLOCK(spe_lock);
static unsigned int spe_nr_entries;

/* Forward declarations */
static void __spe_prune(void);

/*
 * Find an entry by PFN.  Must be called with spe_lock held.
 */
static struct damon_spe_entry *spe_find(unsigned long pfn)
{
	struct rb_node *node = spe_tree.rb_node;

	while (node) {
		struct damon_spe_entry *e =
			rb_entry(node, struct damon_spe_entry, node);

		if (pfn < e->pfn)
			node = node->rb_left;
		else if (pfn > e->pfn)
			node = node->rb_right;
		else
			return e;
	}
	return NULL;
}

/*
 * Insert a new entry.  Must be called with spe_lock held.
 * Returns the new entry, or NULL if the tree is full.
 */
static struct damon_spe_entry *spe_insert(unsigned long pfn, pid_t pid)
{
	struct rb_node **new = &spe_tree.rb_node, *parent = NULL;
	struct damon_spe_entry *e;

	if (spe_nr_entries >= DAMON_SPE_MAX_ENTRIES) {
		__spe_prune();
		if (spe_nr_entries >= DAMON_SPE_MAX_ENTRIES)
			return NULL;
	}

	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return NULL;

	e->pfn = pfn;
	e->pid = pid;
	e->last_access = jiffies;

	while (*new) {
		struct damon_spe_entry *this =
			rb_entry(*new, struct damon_spe_entry, node);

		parent = *new;
		if (pfn < this->pfn)
			new = &((*new)->rb_left);
		else if (pfn > this->pfn)
			new = &((*new)->rb_right);
		else {
			/* Race: another CPU inserted the same PFN */
			kfree(e);
			return this;
		}
	}

	rb_link_node(&e->node, parent, new);
	rb_insert_color(&e->node, &spe_tree);
	spe_nr_entries++;
	return e;
}

/*
 * Prune entries that haven't been updated for DAMON_SPE_ENTRY_TTL.
 * Must be called with spe_lock held.
 */
static void __spe_prune(void)
{
	struct rb_node *node, *next;
	unsigned long deadline = jiffies - DAMON_SPE_ENTRY_TTL;

	node = rb_first(&spe_tree);
	while (node) {
		struct damon_spe_entry *e =
			rb_entry(node, struct damon_spe_entry, node);

		next = rb_next(node);

		if (time_before(e->last_access, deadline)) {
			rb_erase(&e->node, &spe_tree);
			spe_nr_entries--;
			kfree(e);
		}
		node = next;
	}
}

/**
 * damon_spe_record_access() - Record a single subpage access
 * @pfn: Physical page frame number (any page within a THP)
 * @pid: Process ID that performed the access
 *
 * The PFN is automatically aligned to the THP base.  The subpage index
 * within the THP is derived from the low bits of the PFN.
 *
 * Context: Can be called from IRQ context.
 */
void damon_spe_record_access(unsigned long pfn, pid_t pid)
{
	unsigned long thp_pfn = pfn & ~(unsigned long)(DAMON_SPE_MAX_CHUNKS - 1);
	unsigned int idx = pfn & (DAMON_SPE_MAX_CHUNKS - 1);
	struct damon_spe_entry *e;
	unsigned long flags;

	spin_lock_irqsave(&spe_lock, flags);

	e = spe_find(thp_pfn);
	if (!e)
		e = spe_insert(thp_pfn, pid);

	if (e) {
		e->access_count[idx]++;
		e->total_accesses++;
		e->last_access = jiffies;
	}

	spin_unlock_irqrestore(&spe_lock, flags);
}
EXPORT_SYMBOL_GPL(damon_spe_record_access);

/**
 * damon_spe_folio_heatmap() - Get sub-THP access bitmap for a folio
 * @folio: The folio to query
 * @vma: VMA containing the folio
 * @addr: Virtual address of the folio start
 * @target_order: Page order for each chunk in the bitmap
 * @hot_bitmap: Output bitmap with one bit per chunk
 *
 * Queries the SPE rbtree first.  Falls back to PTE access bits if no
 * SPE data is available (requires the THP to be split to PTEs).
 *
 * Return: Number of chunks on success, negative error on failure.
 */
int damon_spe_folio_heatmap(struct folio *folio, struct vm_area_struct *vma,
			    unsigned long addr, unsigned int target_order,
			    unsigned long *hot_bitmap)
{
	unsigned long num_chunks = folio_nr_pages(folio) >> target_order;
	unsigned long chunk_sz = PAGE_SIZE << target_order;
	unsigned long pfn;
	unsigned long flags;
	struct damon_spe_entry *e;
	struct folio_walk fw;
	struct folio *sub_folio;
	int i;

	if (!folio || !vma || !hot_bitmap)
		return -EINVAL;
	if (target_order >= folio_order(folio))
		return -EINVAL;

	pfn = folio_pfn(folio);

	/*
	 * Phase 2b/2c path: query the SPE rbtree.
	 * If we have aggregated SPE data for this folio, use it.
	 */
	spin_lock_irqsave(&spe_lock, flags);
	e = spe_find(pfn);
	if (e && e->total_accesses > 0) {
		unsigned long max_sum = 0;
		unsigned long sig_thresh;
		unsigned int spp = chunk_sz >> PAGE_SHIFT;

		/* First pass: find peak chunk access count */
		for (i = 0; i < num_chunks; i++) {
			unsigned long sum = 0;
			int j;

			for (j = 0; j < spp; j++) {
				unsigned int idx = i * spp + j;

				if (idx < DAMON_SPE_MAX_CHUNKS)
					sum += e->access_count[idx];
			}
			if (sum > max_sum)
				max_sum = sum;
		}

		/*
		 * Signal threshold: a chunk needs >= 1/10 of peak access
		 * count to be considered hot.  This filters SPE noise —
		 * Kunpeng 920 data shows most THPs have scattered 1-2
		 * sample hits across many subpages that don't represent
		 * genuine hot access patterns.
		 */
		sig_thresh = max(max_sum / 10, 1UL);

		/* Second pass: build hot bitmap using signal threshold */
		bitmap_zero(hot_bitmap, num_chunks);
		for (i = 0; i < num_chunks; i++) {
			unsigned long sum = 0;
			int j;

			for (j = 0; j < spp; j++) {
				unsigned int idx = i * spp + j;

				if (idx < DAMON_SPE_MAX_CHUNKS)
					sum += e->access_count[idx];
			}
			if (sum >= sig_thresh)
				__set_bit(i, hot_bitmap);
		}

		spin_unlock_irqrestore(&spe_lock, flags);
		return (int)num_chunks;
	}
	spin_unlock_irqrestore(&spe_lock, flags);

	/*
	 * Phase 2a fallback: walk PTEs to check access bits.
	 * Only works when the THP has been split to PTEs.
	 */
	bitmap_zero(hot_bitmap, num_chunks);

	for (i = 0; i < num_chunks; i++) {
		unsigned long chunk_addr = addr + i * chunk_sz;

		sub_folio = folio_walk_start(&fw, vma, chunk_addr, 0);
		if (!sub_folio)
			return -EOPNOTSUPP;

		if (fw.level == FW_LEVEL_PMD) {
			folio_walk_end(&fw, vma);
			return -EOPNOTSUPP;
		}

		if (fw.level == FW_LEVEL_PTE && pte_young(fw.pte))
			__set_bit(i, hot_bitmap);

		folio_walk_end(&fw, vma);
	}

	return (int)num_chunks;
}
EXPORT_SYMBOL_GPL(damon_spe_folio_heatmap);

/**
 * damon_spe_hot_fraction() - Return hot chunk percentage of a folio
 * @folio: The folio to query
 * @vma: VMA containing the folio
 * @addr: Virtual address of the folio start
 * @target_order: Page order for each chunk
 *
 * Return: Percentage (0-100) on success, negative error on failure.
 */
int damon_spe_hot_fraction(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long addr, unsigned int target_order)
{
	unsigned long num_chunks = folio_nr_pages(folio) >> target_order;
	DECLARE_BITMAP(hot_bitmap, DAMON_SPE_MAX_CHUNKS);
	int ret, hot;

	if (num_chunks > DAMON_SPE_MAX_CHUNKS)
		return -ERANGE;

	ret = damon_spe_folio_heatmap(folio, vma, addr, target_order,
				      hot_bitmap);
	if (ret < 0)
		return ret;

	hot = bitmap_weight(hot_bitmap, num_chunks);
	return (hot * 100) / (int)num_chunks;
}
EXPORT_SYMBOL_GPL(damon_spe_hot_fraction);

/**
 * damon_spe_prune() - Remove stale entries from the SPE rbtree
 *
 * Called from DAMON's aggregation cycle.  Removes entries not updated
 * within DAMON_SPE_ENTRY_TTL jiffies.
 */
void damon_spe_prune(void)
{
	unsigned long flags;

	spin_lock_irqsave(&spe_lock, flags);
	__spe_prune();
	spin_unlock_irqrestore(&spe_lock, flags);
}

/**
 * damon_spe_stats() - Return current SPE rbtree statistics
 * @nr_entries: Output for number of entries, may be NULL
 * @total_accesses: Output for total accumulated accesses, may be NULL
 */
void damon_spe_stats(unsigned int *nr_entries, unsigned long *total_accesses)
{
	struct rb_node *node;
	unsigned long flags;
	unsigned int count = 0;
	unsigned long total = 0;

	spin_lock_irqsave(&spe_lock, flags);
	for (node = rb_first(&spe_tree); node; node = rb_next(node)) {
		struct damon_spe_entry *e =
			rb_entry(node, struct damon_spe_entry, node);
		count++;
		total += e->total_accesses;
	}
	spin_unlock_irqrestore(&spe_lock, flags);

	if (nr_entries)
		*nr_entries = count;
	if (total_accesses)
		*total_accesses = total;
}
EXPORT_SYMBOL_GPL(damon_spe_stats);

/* ---- debugfs interface for Phase 2b (userspace daemon → kernel rbtree) ---- */

static struct dentry *damon_spe_dentry;

/*
 * spe_feed write: accept one PFN per line (hex or decimal).
 * The PFN is recorded as an access via damon_spe_record_access().
 *
 * Usage from userspace:
 *   echo 0x12345678 > /sys/kernel/debug/damon/spe_feed
 *
 * For bulk feed from SPE daemon:
 *   cat spe_pfns.txt > /sys/kernel/debug/damon/spe_feed
 */
static ssize_t spe_feed_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	char line[32];
	size_t len = min(count, sizeof(line) - 1);
	unsigned long pfn;

	if (copy_from_user(line, buf, len))
		return -EFAULT;
	line[len] = '\0';

	/* Strip trailing newline */
	if (len > 0 && line[len - 1] == '\n')
		line[len - 1] = '\0';

	if (kstrtoul(line, 0, &pfn) == 0 && pfn != 0)
		damon_spe_record_access(pfn, 0);

	return count;
}

/*
 * spe_stats read: show current SPE rbtree statistics.
 *
 * Usage:
 *   cat /sys/kernel/debug/damon/spe_stats
 */
static int spe_stats_show(struct seq_file *m, void *v)
{
	struct rb_node *node;
	unsigned long flags;
	unsigned int count = 0;
	unsigned long total = 0;

	spin_lock_irqsave(&spe_lock, flags);
	for (node = rb_first(&spe_tree); node; node = rb_next(node)) {
		struct damon_spe_entry *e =
			rb_entry(node, struct damon_spe_entry, node);
		count++;
		total += e->total_accesses;
	}
	spin_unlock_irqrestore(&spe_lock, flags);

	seq_printf(m, "nr_entries=%u total_accesses=%lu\n", count, total);

	/* Show top entries (limit output) */
	spin_lock_irqsave(&spe_lock, flags);
	count = 0;
	for (node = rb_first(&spe_tree); node; node = rb_next(node)) {
		struct damon_spe_entry *e =
			rb_entry(node, struct damon_spe_entry, node);
		unsigned int hot_pages = 0;
		int i;

		for (i = 0; i < DAMON_SPE_MAX_CHUNKS; i++)
			if (e->access_count[i])
				hot_pages++;

		seq_printf(m, "  pfn=0x%lx pid=%d total=%lu hot_pages=%u/%d\n",
			   e->pfn, e->pid, e->total_accesses,
			   hot_pages, DAMON_SPE_MAX_CHUNKS);
		if (++count >= 10)
			break;
	}
	spin_unlock_irqrestore(&spe_lock, flags);

	return 0;
}

static int spe_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, spe_stats_show, inode->i_private);
}

static const struct file_operations spe_feed_fops = {
	.write = spe_feed_write,
};

static const struct file_operations spe_stats_fops = {
	.open = spe_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init damon_spe_dbgfs_init(void)
{
	struct dentry *d;

	d = debugfs_lookup("damon", NULL);
	if (!d) {
		d = debugfs_create_dir("damon", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}
	damon_spe_dentry = d;

	debugfs_create_file("spe_feed", 0200, damon_spe_dentry,
			    NULL, &spe_feed_fops);
	debugfs_create_file("spe_stats", 0400, damon_spe_dentry,
			    NULL, &spe_stats_fops);

	pr_info("debugfs interface ready: /sys/kernel/debug/damon/spe_{feed,stats}\n");
	return 0;
}

late_initcall(damon_spe_dbgfs_init);
