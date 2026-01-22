/* SPDX-License-Identifier: GPL-2.0 */

#if !defined(_TRACE_TEGRA_VPR_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TEGRA_VPR_H

#undef TRACE_SYSTEM
#define TRACE_SYSTEM tegra_vpr

#include <linux/tracepoint.h>

TRACE_EVENT(tegra_vpr_chunk_activate,
	TP_PROTO(phys_addr_t start, phys_addr_t limit),
	TP_ARGS(start, limit),
	TP_STRUCT__entry(
		__field(phys_addr_t, start)
		__field(phys_addr_t, limit)
	),
	TP_fast_assign(
		__entry->start = start;
		__entry->limit = limit;
	),
	TP_printk("%pap-%pap", &__entry->start,
		  &__entry->limit)
);

TRACE_EVENT(tegra_vpr_chunk_deactivate,
	TP_PROTO(phys_addr_t start, phys_addr_t limit),
	TP_ARGS(start, limit),
	TP_STRUCT__entry(
		__field(phys_addr_t, start)
		__field(phys_addr_t, limit)
	),
	TP_fast_assign(
		__entry->start = start;
		__entry->limit = limit;
	),
	TP_printk("%pap-%pap", &__entry->start,
		  &__entry->limit)
);

TRACE_EVENT(tegra_vpr_set,
	TP_PROTO(phys_addr_t base, phys_addr_t size),
	TP_ARGS(base, size),
	TP_STRUCT__entry(
		__field(phys_addr_t, start)
		__field(phys_addr_t, limit)
	),
	TP_fast_assign(
		__entry->start = base;
		__entry->limit = base + size;
	),
	TP_printk("%pap-%pap", &__entry->start, &__entry->limit)
);

#endif /* _TRACE_TEGRA_VPR_H */

#include <trace/define_trace.h>
