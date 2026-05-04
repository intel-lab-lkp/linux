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

extern unsigned int pghot_target_nid;
extern unsigned int pghot_src_enabled;
extern unsigned int pghot_freq_threshold;
extern unsigned int kmigrated_sleep_ms;
extern unsigned int kmigrated_batch_nr;
extern unsigned int sysctl_pghot_freq_window;

void pghot_debug_init(void);

DECLARE_STATIC_KEY_FALSE(pghot_src_hintfaults);
DECLARE_STATIC_KEY_FALSE(pghot_src_hwhints);

#define PGHOT_HINTFAULTS_ENABLED	BIT(PGHOT_HINTFAULTS)
#define PGHOT_HWHINTS_ENABLED		BIT(PGHOT_HWHINTS)
#define PGHOT_SRC_ENABLED_MASK		GENMASK(PGHOT_SRC_MAX - 1, 0)

#define PGHOT_DEFAULT_FREQ_THRESHOLD	2

#define KMIGRATED_DEFAULT_SLEEP_MS	100
#define KMIGRATED_DEFAULT_BATCH_NR	512

#define PGHOT_DEFAULT_NODE		0

#define PGHOT_DEFAULT_FREQ_WINDOW	(3 * MSEC_PER_SEC)

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

typedef u8 phi_t;

#define PGHOT_RECORD_SIZE		sizeof(phi_t)

#define PGHOT_SECTION_HOT_BIT		0
#define PGHOT_SECTION_HOT_MASK		BIT(PGHOT_SECTION_HOT_BIT)

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
