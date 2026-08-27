/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-edge dwell-time statistics for RV monitors.
 *
 * Copyright (C) 2026 Siemens AG
 * Author: Tobias Schaffner <tobias.schaffner@siemens.com>
 */
#ifndef _LINUX_RV_EDGE_STAT_H
#define _LINUX_RV_EDGE_STAT_H

#include <linux/compiler.h>
#include <linux/percpu.h>
#include <linux/rv.h>
#include <linux/types.h>
#include <asm/local64.h>

struct rv_edge_stat {
	local64_t	count;
	local64_t	sum_ns;
	local64_t	max_ns;
};

static __always_inline
void rv_edge_stat_account(struct rv_edge_stat *s, u64 dwell_ns)
{
	s64 max;

	local64_inc(&s->count);
	local64_add(dwell_ns, &s->sum_ns);

	/* Keep the largest dwell; retry only if a nested update raced us. */
	max = local64_read(&s->max_ns);
	while (dwell_ns > (u64)max) {
		s64 prev = local64_cmpxchg(&s->max_ns, max, dwell_ns);

		if (prev == max)
			break;
		max = prev;
	}
}

#ifdef CONFIG_RV_EDGE_STAT
/**
 * rv_edge_account - record a dwell of @dwell_ns on @edge of monitor @mon
 *
 * Cheap and lock-free: the local64_t counters make this safe against interrupt
 * and NMI nesting on the current CPU without disabling interrupts, so it does
 * not perturb the latency being measured. The caller only needs to stay on its
 * CPU for the call (as tracepoint probes already do).
 */
static __always_inline void
rv_edge_account(struct rv_monitor *mon, unsigned int edge, u64 dwell_ns)
{
	struct rv_edge_stat *e = this_cpu_ptr(mon->edge_pcpu);

	rv_edge_stat_account(&e[edge], dwell_ns);
}
#endif /* CONFIG_RV_EDGE_STAT */

#endif /* _LINUX_RV_EDGE_STAT_H */
