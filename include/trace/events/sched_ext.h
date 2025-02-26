/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM sched_ext

#if !defined(_TRACE_SCHED_EXT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SCHED_EXT_H

#include <linux/tracepoint.h>

TRACE_EVENT(sched_ext_dump,

	TP_PROTO(const char *line),

	TP_ARGS(line),

	TP_STRUCT__entry(
		__string(line, line)
	),

	TP_fast_assign(
		__assign_str(line);
	),

	TP_printk("%s",
		__get_str(line)
	)
);

TRACE_EVENT(sched_ext_add_event,
	    TP_PROTO(const char *name, int offset, __u64 added),
	    TP_ARGS(name, offset, added),

	TP_STRUCT__entry(
		__string(name, name)
		__field(	int,		offset		)
		__field(	__u64,		added		)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->offset		= offset;
		__entry->added		= added;
	),

	TP_printk("name %s offset %d added %llu",
		  __get_str(name), __entry->offset, __entry->added
	)
);

#endif /* _TRACE_SCHED_EXT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
