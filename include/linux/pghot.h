/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PGHOT_H
#define _LINUX_PGHOT_H

/* Page hotness temperature sources */
enum pghot_src {
	PGHOT_HINTFAULTS = 0,
	PGHOT_HWHINTS,
	PGHOT_SRC_MAX
};

#ifdef CONFIG_PGHOT
#include <linux/static_key.h>
#include <linux/mutex.h>

extern unsigned int sysctl_pghot_target_nid;
extern unsigned int sysctl_pghot_freq_window;
extern unsigned int sysctl_pghot_freq_threshold;
extern struct mutex pghot_tunables_lock;

DECLARE_STATIC_KEY_FALSE(pghot_src_hintfaults);
DECLARE_STATIC_KEY_FALSE(pghot_src_hwhints);

#define PGHOT_HINTFAULTS_ENABLED	BIT(PGHOT_HINTFAULTS)
#define PGHOT_HWHINTS_ENABLED		BIT(PGHOT_HWHINTS)
#define PGHOT_SRC_ENABLED_MASK		GENMASK(PGHOT_SRC_MAX - 1, 0)

#define PGHOT_FREQ_THRESHOLD_DEFAULT	2

#define KMIGRATED_SLEEP_MS_MIN		50
#define KMIGRATED_SLEEP_MS_DEFAULT	100
#define KMIGRATED_SLEEP_MS_MAX		1000

#define KMIGRATED_BATCH_NR_MIN		64
#define KMIGRATED_BATCH_NR_DEFAULT	512
#define KMIGRATED_BATCH_NR_MAX		1024

#define PGHOT_DEFAULT_NODE		0

#if defined(CONFIG_PGHOT_PRECISE)
#define PGHOT_FREQ_WINDOW_MIN		(1 * MSEC_PER_SEC)
#define PGHOT_FREQ_WINDOW_DEFAULT	(5 * MSEC_PER_SEC)

/*
 * Bits 0-26 are used to store nid, frequency and time.
 * Bits 27-30 are unused now.
 * Bit 31 is used to indicate the page is ready for migration.
 */
#define PGHOT_MIGRATE_READY		31

#define PGHOT_NID_WIDTH			10
#define PGHOT_FREQ_WIDTH		3
/* time is stored in 14 bits which can represent up to 16s with HZ=1000 */
#define PGHOT_TIME_WIDTH		14

#define PGHOT_NID_SHIFT			0
#define PGHOT_FREQ_SHIFT		(PGHOT_NID_SHIFT + PGHOT_NID_WIDTH)
#define PGHOT_TIME_SHIFT		(PGHOT_FREQ_SHIFT + PGHOT_FREQ_WIDTH)

#define PGHOT_NID_MASK			GENMASK(PGHOT_NID_WIDTH - 1, 0)
#define PGHOT_FREQ_MASK			GENMASK(PGHOT_FREQ_WIDTH - 1, 0)
#define PGHOT_TIME_MASK			GENMASK(PGHOT_TIME_WIDTH - 1, 0)

#define PGHOT_NID_MAX			((1 << PGHOT_NID_WIDTH) - 1)
#define PGHOT_FREQ_MAX			((1 << PGHOT_FREQ_WIDTH) - 1)
#define PGHOT_TIME_MAX			((1 << PGHOT_TIME_WIDTH) - 1)
#define PGHOT_FREQ_WINDOW_MAX		PGHOT_TIME_MAX

typedef u32 phi_t;

#else	/* !CONFIG_PGHOT_PRECISE */
#define PGHOT_FREQ_WINDOW_MIN		(1 * MSEC_PER_SEC)
#define PGHOT_FREQ_WINDOW_DEFAULT       (3 * MSEC_PER_SEC)

/*
 * Bits 0-6 are used to store frequency and time.
 * Bit 7 is used to indicate the page is ready for migration.
 */
#define PGHOT_MIGRATE_READY		7

#define PGHOT_FREQ_WIDTH		2
/* Bucketed time is stored in 5 bits which can represent up to 3.9s with HZ=1000 */
#define PGHOT_TIME_BUCKETS_SHIFT	7
#define PGHOT_TIME_WIDTH		5
#define PGHOT_NID_WIDTH			10

#define PGHOT_FREQ_SHIFT		0
#define PGHOT_TIME_SHIFT		(PGHOT_FREQ_SHIFT + PGHOT_FREQ_WIDTH)

#define PGHOT_FREQ_MASK			GENMASK(PGHOT_FREQ_WIDTH - 1, 0)
#define PGHOT_TIME_MASK			GENMASK(PGHOT_TIME_WIDTH - 1, 0)
#define PGHOT_TIME_BUCKETS_MASK		(PGHOT_TIME_MASK << PGHOT_TIME_BUCKETS_SHIFT)

#define PGHOT_NID_MAX			((1 << PGHOT_NID_WIDTH) - 1)
#define PGHOT_FREQ_MAX			((1 << PGHOT_FREQ_WIDTH) - 1)
#define PGHOT_TIME_MAX			((1 << PGHOT_TIME_WIDTH) - 1)
#define PGHOT_FREQ_WINDOW_MAX		PGHOT_TIME_BUCKETS_MASK

typedef u8 phi_t;

static_assert(MAX_NUMNODES <= (1 << PGHOT_NID_WIDTH),
	      "pghot precise nid field too narrow for MAX_NUMNODES");

#endif /* CONFIG_PGHOT_PRECISE */

#define PGHOT_RECORD_SIZE		sizeof(phi_t)

#define PGHOT_SECTION_HOT_BIT		0

/**
 * struct pghot_hot_map - per-section hotness map
 * @rcu:   used to free the map after an RCU grace period via kfree_rcu().
 * @flags: section-level flags; currently only PGHOT_SECTION_HOT_BIT, set
 *         when the section contains at least one page classified as hot
 *         and consumed by the kmigrated thread.
 * @phi:   per-PFN hotness records, PAGES_PER_SECTION entries.
 *
 * Referenced from struct mem_section through an RCU-protected pointer
 * (mem_section->hot_map). Readers must dereference ->hot_map under
 * rcu_read_lock() and updaters must use the RCU pointer accessors.
 */
struct pghot_hot_map {
	struct rcu_head rcu;
	unsigned long flags;
	phi_t phi[];
};

bool pghot_nid_valid(int nid);
unsigned long pghot_access_latency(unsigned long old_time, unsigned long time);
bool pghot_update_record(phi_t *phi, int nid, unsigned long now);
int pghot_get_record(phi_t *phi, int *nid, int *freq, unsigned long *time);

int pghot_record_access(unsigned long pfn, int nid, int src, unsigned long now);
#else
static inline int pghot_record_access(unsigned long pfn, int nid, int src, unsigned long now)
{
	return 0;
}
#endif /* CONFIG_PGHOT */
#endif /* _LINUX_PGHOT_H */
