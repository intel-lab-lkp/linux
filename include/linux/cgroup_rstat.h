/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RSTAT_H
#define _LINUX_RSTAT_H

#include <linux/u64_stats_sync.h>

struct cgroup_rstat_cpu;

/*
 * rstat - cgroup scalable recursive statistics.  Accounting is done
 * per-cpu in cgroup_rstat_cpu which is then lazily propagated up the
 * hierarchy on reads.
 *
 * When a stat gets updated, the cgroup_rstat_cpu and its ancestors are
 * linked into the updated tree.  On the following read, propagation only
 * considers and consumes the updated tree.  This makes reading O(the
 * number of descendants which have been active since last read) instead of
 * O(the total number of descendants).
 *
 * This is important because there can be a lot of (draining) cgroups which
 * aren't active and stat may be read frequently.  The combination can
 * become very expensive.  By propagating selectively, increasing reading
 * frequency decreases the cost of each read.
 *
 * This struct hosts both the fields which implement the above -
 * updated_children and updated_next - and the fields which track basic
 * resource statistics on top of it - bsync, bstat and last_bstat.
 */
struct cgroup_rstat {
	struct cgroup_rstat_cpu __percpu *rstat_cpu;

	/*
	 * Add padding to separate the read mostly rstat_cpu and
	 * rstat_css_list into a different cacheline from the following
	 * rstat_flush_next and containing struct fields which can have
	 * frequent updates.
	 */
	CACHELINE_PADDING(_pad_);
	struct cgroup_rstat *rstat_flush_next;
};

struct cgroup_base_stat {
	struct task_cputime cputime;

#ifdef CONFIG_SCHED_CORE
	u64 forceidle_sum;
#endif
	u64 ntime;
};

struct cgroup_rstat_cpu {
	/*
	 * Child cgroups with stat updates on this cpu since the last read
	 * are linked on the parent's ->updated_children through
	 * ->updated_next.
	 *
	 * In addition to being more compact, singly-linked list pointing
	 * to the cgroup makes it unnecessary for each per-cpu struct to
	 * point back to the associated cgroup.
	 */
	struct cgroup_rstat *updated_children;	/* terminated by self */
	struct cgroup_rstat *updated_next;		/* NULL if not on the list */

	/*
	 * ->bsync protects ->bstat.  These are the only fields which get
	 * updated in the hot path.
	 */
	struct u64_stats_sync bsync;
	struct cgroup_base_stat bstat;

	/*
	 * Snapshots at the last reading.  These are used to calculate the
	 * deltas to propagate to the global counters.
	 */
	struct cgroup_base_stat last_bstat;

	/*
	 * This field is used to record the cumulative per-cpu time of
	 * the cgroup and its descendants. Currently it can be read via
	 * eBPF/drgn etc, and we are still trying to determine how to
	 * expose it in the cgroupfs interface.
	 */
	struct cgroup_base_stat subtree_bstat;

	/*
	 * Snapshots at the last reading. These are used to calculate the
	 * deltas to propagate to the per-cpu subtree_bstat.
	 */
	struct cgroup_base_stat last_subtree_bstat;
};

#endif	/* _LINUX_RSTAT_H */
