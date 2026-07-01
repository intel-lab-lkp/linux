/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fetcharg_bench

#if !defined(_FETCHARG_BENCH_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _FETCHARG_BENCH_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(fetcharg_bench_event,

	TP_PROTO(int a, int **b, char *c),

	TP_ARGS(a, b, c),

	TP_STRUCT__entry(
		__field(int, a)
		__field(int **, b_ptr)
		__field(char *, c_ptr)
	),

	TP_fast_assign(
		__entry->a = a;
		__entry->b_ptr = b;
		__entry->c_ptr = c;
	),

	TP_printk("a=%d b=%p c=%p", __entry->a, __entry->b_ptr, __entry->c_ptr)
);

#endif /* _FETCHARG_BENCH_TRACE_H */

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE fetcharg_bench_trace
#include <trace/define_trace.h>
