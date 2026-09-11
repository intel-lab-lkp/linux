/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-edge dwell-time statistics for RV monitors.
 *
 * Copyright (C) 2026 Siemens AG
 * Author: Tobias Schaffner <tobias.schaffner@siemens.com>
 */
#ifndef _RV_EDGE_STAT_H
#define _RV_EDGE_STAT_H

#include <linux/args.h>
#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/percpu.h>
#include <linux/rv.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/tracefs.h>
#include <linux/types.h>
#include <asm/local64.h>
#include <rv/automata.h>

#ifdef CONFIG_RV_EDGE_STAT

/*
 * Per-CPU counters kept in local64_t so accounting is safe against interrupt
 * and NMI nesting on the owning CPU without disabling interrupts. Only the
 * owning CPU writes.
 */
struct rv_edge_stat {
	local64_t	count;
	local64_t	sum_ns;
	local64_t	max_ns;
};

/*
 * State and entry timestamp share one word so a transition commits both with
 * one cmpxchg. Comparing the timestamp also detects nested transitions that
 * return to the same state.
 */
#define RV_STATE_BITS		8
#define RV_STATE_MASK		GENMASK(RV_STATE_BITS - 1, 0)
#define RV_TS_MASK		GENMASK(BITS_PER_LONG - RV_STATE_BITS - 1, 0)

#define da_state_of(w)		((unsigned int)((w) & RV_STATE_MASK))
#define da_ts_of(w)		((u64)(w) >> RV_STATE_BITS)
#define da_state_pack(s, ts)	(((((da_state_t)(ts)) & RV_TS_MASK) << RV_STATE_BITS) | \
				 ((da_state_t)(s) & RV_STATE_MASK))

static __always_inline
void rv_edge_stat_account(struct rv_edge_stat *s, u64 dwell_ns)
{
	s64 max;
	int i;

	local64_inc(&s->count);
	local64_add(dwell_ns, &s->sum_ns);

	/* Keep the largest dwell; bound retries if a nested update races us. */
	max = local64_read(&s->max_ns);
	for (i = 0; dwell_ns > (u64)max; i++) {
		if (i == MAX_DA_RETRY_RACING_EVENTS) {
			WARN_ONCE(1, "rv: edge-stat max update exceeded %d retries\n",
				  MAX_DA_RETRY_RACING_EVENTS);
			break;
		}
		if (local64_try_cmpxchg(&s->max_ns, &max, dwell_ns))
			break;
	}
}

#define DA_MON_EDGES CONCATENATE(da_mon_edges_, MONITOR_NAME)

static_assert(STATE_MAX <= (1U << RV_STATE_BITS),
	      "automaton has more states than the packed state word can hold");

#if RV_MON_TYPE == RV_MON_PER_CPU

/*
 * Static storage avoids allocation lifetime races. Only the owning CPU writes,
 * so readers can snapshot counters with local64_read().
 */
#define RV_THIS_NR_EDGES	(STATE_MAX * EVENT_MAX)

struct rv_this_edges {
	struct rv_edge_stat	edge[RV_THIS_NR_EDGES];
};

static DEFINE_PER_CPU(struct rv_this_edges, DA_MON_EDGES);
static struct dentry *rv_this_stats_file;

/* Called before tracepoints are registered, so memset cannot race an update. */
static void rv_edge_stats_reset(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		memset(per_cpu_ptr(&DA_MON_EDGES, cpu), 0,
		       sizeof(struct rv_this_edges));
}

static int rv_edge_stats_show(struct seq_file *seq, void *v)
{
	unsigned int e;
	int cpu;

	seq_puts(seq, "# cpu edge label count sum_ns max_ns\n");
	/* Include offline CPUs and keep output stable across CPU hotplug. */
	for_each_possible_cpu(cpu) {
		struct rv_this_edges *m = per_cpu_ptr(&DA_MON_EDGES, cpu);

		for (e = 0; e < RV_THIS_NR_EDGES; e++)
			seq_printf(seq, "%d %u %s:%s %llu %llu %llu\n",
				   cpu, e,
				   model_get_state_name(e / EVENT_MAX),
				   model_get_event_name(e % EVENT_MAX),
				   (u64)local64_read(&m->edge[e].count),
				   (u64)local64_read(&m->edge[e].sum_ns),
				   (u64)local64_read(&m->edge[e].max_ns));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rv_edge_stats);

static int rv_edge_stats_create(struct dentry *parent)
{
	rv_edge_stats_reset();
	/* RV_MODE_READ is private to kernel/trace/rv/. */
	rv_this_stats_file = tracefs_create_file("stats", 0440, parent,
						 NULL, &rv_edge_stats_fops);
	return rv_this_stats_file ? 0 : -ENOMEM;
}

static void rv_edge_stats_remove(void)
{
	tracefs_remove(rv_this_stats_file);
	rv_this_stats_file = NULL;
}

/* A zero timestamp marks resets, which can run on a different CPU. */
#define da_state_entered(s)	da_state_pack((s), local_clock() & RV_TS_MASK)

static __always_inline void
rv_edge_account(da_state_t old, da_state_t new, enum states curr, enum events ev)
{
	struct rv_this_edges *m;
	u64 prev = da_ts_of(old);

	if (!prev)
		return;

	m = this_cpu_ptr(&DA_MON_EDGES);
	rv_edge_stat_account(&m->edge[curr * EVENT_MAX + ev],
			     (da_ts_of(new) - prev) & RV_TS_MASK);
}

#else /* per-cpu accounting off for this monitor type */

#define da_state_entered(s)	((da_state_t)(s))

static __always_inline void
rv_edge_account(da_state_t old, da_state_t new, enum states curr, enum events ev) { }

#endif /* RV_MON_TYPE == RV_MON_PER_CPU */

static __always_inline bool
da_state_try_commit(da_state_t *word, da_state_t *old, enum states next,
		    enum events event)
{
	da_state_t prev = *old;
	da_state_t new = da_state_entered(next);

	if (!try_cmpxchg(word, old, new))
		return false;

	rv_edge_account(prev, new, da_state_of(prev), event);
	return true;
}

#else /* !CONFIG_RV_EDGE_STAT */

/*
 * Feature off: only the hooks the DA/HA layer calls are provided, as trivial
 * pass-throughs. da_state_t is a plain state word with no packed timestamp.
 */
#define da_state_of(w)		((unsigned int)(w))
#define da_state_entered(s)	(s)

static __always_inline bool
da_state_try_commit(da_state_t *word, da_state_t *old, enum states next,
		    enum events event)
{
	return try_cmpxchg(word, old, next);
}

#if RV_MON_TYPE == RV_MON_PER_CPU
static inline int rv_edge_stats_create(struct dentry *parent) { return 0; }
static inline void rv_edge_stats_remove(void) { }
#endif

#endif /* CONFIG_RV_EDGE_STAT */

#endif /* _RV_EDGE_STAT_H */
