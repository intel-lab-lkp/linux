/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM memcg

#if !defined(_TRACE_MEMCG_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MEMCG_H

#include <linux/memcontrol.h>
#include <linux/tracepoint.h>

#define MEMCG_FLUSH_REASONS \
	EM(TRACE_MEMCG_FLUSH_READER, "reader") \
	EM(TRACE_MEMCG_FLUSH_READER_SKIP, "reader skip") \
	EM(TRACE_MEMCG_FLUSH_PERIODIC, "periodic") \
	EMe(TRACE_MEMCG_FLUSH_ZSWAP, "zswap")

#ifndef __MEMCG_DECLARE_TRACE_ENUMS_ONLY_ONCE
#define __MEMCG_DECLARE_TRACE_ENUMS_ONLY_ONCE

/* Redefine macros to help declare enum */
#undef EM
#undef EMe
#define EM(a, b)	a,
#define EMe(a, b)	a

enum memcg_flush_reason {
	MEMCG_FLUSH_REASONS
};

#endif /* __MEMCG_DECLARE_TRACE_ENUMS_ONLY_ONCE */

/* Redefine macros to export the enums to userspace */
#undef EM
#undef EMe
#define EM(a, b)	TRACE_DEFINE_ENUM(a);
#define EMe(a, b)	TRACE_DEFINE_ENUM(a)

MEMCG_FLUSH_REASONS;

/*
 * Redefine macros to map the enums to the strings that will
 * be printed in the output
 */
#undef EM
#undef EMe
#define EM(a, b)	{ a, b },
#define EMe(a, b)	{ a, b }

DECLARE_EVENT_CLASS(memcg_rstat_stats,

	TP_PROTO(struct mem_cgroup *memcg, int item, int val),

	TP_ARGS(memcg, item, val),

	TP_STRUCT__entry(
		__field(u64, id)
		__field(int, item)
		__field(int, val)
	),

	TP_fast_assign(
		__entry->id = cgroup_id(memcg->css.cgroup);
		__entry->item = item;
		__entry->val = val;
	),

	TP_printk("memcg_id=%llu item=%d val=%d",
		  __entry->id, __entry->item, __entry->val)
);

DEFINE_EVENT(memcg_rstat_stats, mod_memcg_state,

	TP_PROTO(struct mem_cgroup *memcg, int item, int val),

	TP_ARGS(memcg, item, val)
);

DEFINE_EVENT(memcg_rstat_stats, mod_memcg_lruvec_state,

	TP_PROTO(struct mem_cgroup *memcg, int item, int val),

	TP_ARGS(memcg, item, val)
);

DECLARE_EVENT_CLASS(memcg_rstat_events,

	TP_PROTO(struct mem_cgroup *memcg, int item, unsigned long val),

	TP_ARGS(memcg, item, val),

	TP_STRUCT__entry(
		__field(u64, id)
		__field(int, item)
		__field(unsigned long, val)
	),

	TP_fast_assign(
		__entry->id = cgroup_id(memcg->css.cgroup);
		__entry->item = item;
		__entry->val = val;
	),

	TP_printk("memcg_id=%llu item=%d val=%lu",
		  __entry->id, __entry->item, __entry->val)
);

DEFINE_EVENT(memcg_rstat_events, count_memcg_events,

	TP_PROTO(struct mem_cgroup *memcg, int item, unsigned long val),

	TP_ARGS(memcg, item, val)
);

TRACE_EVENT(memcg_flush_stats,

	TP_PROTO(struct mem_cgroup *memcg, enum memcg_flush_reason reason),

	TP_ARGS(memcg, reason),

	TP_STRUCT__entry(
		__field(u64, id)
		__field(enum memcg_flush_reason, reason)
	),

	TP_fast_assign(
		__entry->id = cgroup_id(memcg->css.cgroup);
		__entry->reason = reason;
	),

	TP_printk("memcg_id=%llu reason=%s",
		  __entry->id, __print_symbolic(__entry->reason, MEMCG_FLUSH_REASONS))
);

#endif /* _TRACE_MEMCG_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
