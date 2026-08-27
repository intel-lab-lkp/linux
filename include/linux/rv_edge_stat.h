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
#include <linux/types.h>
#include <asm/local64.h>

/*
 * Per-CPU counters kept in local64_t so accounting is safe against interrupt
 * and NMI nesting on the owning CPU without disabling interrupts -- the same
 * approach the trace ring buffer uses. Only the owning CPU writes.
 */
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

#endif /* _LINUX_RV_EDGE_STAT_H */
