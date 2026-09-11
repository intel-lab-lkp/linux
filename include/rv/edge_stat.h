/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-edge dwell-time statistics for RV monitors.
 *
 * Copyright (C) 2026 Siemens AG
 * Author: Tobias Schaffner <tobias.schaffner@siemens.com>
 */
#ifndef _RV_EDGE_STAT_H
#define _RV_EDGE_STAT_H

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/rv.h>
#include <linux/types.h>
#include <asm/local64.h>

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

#endif /* CONFIG_RV_EDGE_STAT */

#endif /* _RV_EDGE_STAT_H */
