/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WORKINGSET_REPORT_H
#define _LINUX_WORKINGSET_REPORT_H

#include <linux/types.h>
#include <linux/mutex.h>

struct mem_cgroup;
struct pglist_data;
struct node;
struct lruvec;
struct cgroup_file;

#ifdef CONFIG_WORKINGSET_REPORT

#define WORKINGSET_REPORT_MIN_NR_BINS 2
#define WORKINGSET_REPORT_MAX_NR_BINS 32

#define WORKINGSET_INTERVAL_MAX ((unsigned long)-1)
#define ANON_AND_FILE 2

struct wsr_report_bin {
	unsigned long idle_age;
	unsigned long nr_pages[ANON_AND_FILE];
};

struct wsr_report_bins {
	/* excludes the WORKINGSET_INTERVAL_MAX bin */
	unsigned long nr_bins;
	/* last bin contains WORKINGSET_INTERVAL_MAX */
	unsigned long idle_age[WORKINGSET_REPORT_MAX_NR_BINS];
	struct rcu_head rcu;
};

struct wsr_page_age_histo {
	unsigned long timestamp;
	struct wsr_report_bin bins[WORKINGSET_REPORT_MAX_NR_BINS];
};

struct wsr_state {
	unsigned long report_threshold;
	unsigned long refresh_interval;

	union {
		struct kernfs_node *page_age_sys_file;
		struct cgroup_file *page_age_cgroup_file;
	};

	/* breakdown of workingset by page age */
	struct mutex page_age_lock;
	struct wsr_page_age_histo *page_age;
};

void wsr_init_lruvec(struct lruvec *lruvec);
void wsr_destroy_lruvec(struct lruvec *lruvec);
void wsr_init_pgdat(struct pglist_data *pgdat);
void wsr_destroy_pgdat(struct pglist_data *pgdat);
void wsr_init_sysfs(struct node *node);
void wsr_remove_sysfs(struct node *node);

/*
 * Returns true if the wsr is configured to be refreshed.
 * The next refresh time is stored in refresh_time.
 */
bool wsr_refresh_report(struct wsr_state *wsr, struct mem_cgroup *root,
			struct pglist_data *pgdat);
#else
static inline void wsr_init_lruvec(struct lruvec *lruvec)
{
}
static inline void wsr_destroy_lruvec(struct lruvec *lruvec)
{
}
static inline void wsr_init_pgdat(struct pglist_data *pgdat)
{
}
static inline void wsr_destroy_pgdat(struct pglist_data *pgdat)
{
}
static inline void wsr_init_sysfs(struct node *node)
{
}
static inline void wsr_remove_sysfs(struct node *node)
{
}
#endif /* CONFIG_WORKINGSET_REPORT */

#endif /* _LINUX_WORKINGSET_REPORT_H */
