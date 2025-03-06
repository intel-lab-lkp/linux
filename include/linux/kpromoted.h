/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KPROMOTED_H
#define _LINUX_KPROMOTED_H

#include <linux/types.h>
#include <linux/init.h>
#include <linux/workqueue_types.h>

/* Page hotness temperature sources */
enum kpromoted_src {
	KPROMOTED_HW_HINTS,
	KPROMOTED_PGTABLE_SCAN,
};

#ifdef CONFIG_KPROMOTED

#define KPROMOTED_FREQ_WINDOW	(5 * MSEC_PER_SEC)

/* 2 accesses within a window will make the page a promotion candidate */
#define KPRMOTED_FREQ_THRESHOLD	2

#define KPROMOTED_HASH_ORDER	16

struct page_hotness_info {
	unsigned long pfn;

	/* Time when this record was updated last */
	unsigned long last_update;

	/*
	 * Number of times this page was accessed in the
	 * current window
	 */
	int frequency;

	/* Most recent access time */
	unsigned long recency;

	/* Most recent access from this node */
	int hot_node;
	struct hlist_node hnode;
};

#define KPROMOTE_DELAY	MSEC_PER_SEC

int kpromoted_record_access(u64 pfn, int nid, int src, unsigned long now);
#else
static inline int kpromoted_record_access(u64 pfn, int nid, int src,
					  unsigned long now)
{
	return 0;
}
#endif /* CONFIG_KPROMOTED */
#endif /* _LINUX_KPROMOTED_H */
