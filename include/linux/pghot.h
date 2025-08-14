/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KPROMOTED_H
#define _LINUX_KPROMOTED_H

#include <linux/types.h>
#include <linux/init.h>
#include <linux/workqueue_types.h>

/* Page hotness temperature sources */
enum pghot_src {
	PGHOT_HW_HINTS,
	PGHOT_PGTABLE_SCAN,
};

#ifdef CONFIG_PGHOT

#define KPROMOTED_FREQ_WINDOW	(5 * MSEC_PER_SEC)

/* 2 accesses within a window will make the page a promotion candidate */
#define KPRMOTED_FREQ_THRESHOLD	2

/*
 * The following two defines control the number of hash lists
 * that are maintained for tracking PFN accesses.
 */
#define PGHOT_HASH_PCT		50	/* % of lower tier memory pages to track */
#define PGHOT_HASH_ENTRIES	1024	/* Number of entries per list, ideal case */

/*
 * Percentage of hash entries that can reside in heap as migrate-ready
 * candidates
 */
#define PGHOT_HEAP_PCT		25

#define KPRMOTED_MIGRATE_BATCH	1024

/*
 * If target NID isn't available, kpromoted promotes to node 0
 * by default.
 *
 * TODO: Need checks to validate that default node is indeed
 * present and is a toptier node.
 */
#define KPROMOTED_DEFAULT_NODE	0

struct pghot_info {
	unsigned long pfn;

	/*
	 * The following are the three fundamental parameters
	 * required to track the hotness of page/PFN.
	 *
	 * TODO:
	 * Check if these three can fit into a u32.
	 * With 3 bits for frequency (8 most recent accesses),
	 * 10 bits for nid (1024 nodes), the remaining 19 bits
	 * are available for timestamp.
	 */
	unsigned long last_update; /* Most recent access time */
	int frequency; /* Number of accesses within current window */
	int nid; /* Most recent access from this node */

	struct hlist_node hnode;
	size_t heap_idx; /* Position in max heap for quick retreival */
};

struct max_heap {
	size_t nr;
	size_t size;
	struct pghot_info **data;
	DECLARE_FLEX_ARRAY(struct pghot_info *, preallocated);
};

/*
 * The wakeup interval of kpromoted threads
 */
#define KPROMOTE_DELAY	20	/* 20ms */

int pghot_record_access(u64 pfn, int nid, int src, unsigned long now);
#else
static inline int pghot_record_access(u64 pfn, int nid, int src,
				      unsigned long now)
{
	return 0;
}
#endif /* CONFIG_PGHOT */
#endif /* _LINUX_KPROMOTED_H */
