/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM vma

#if !defined(_TRACE_VMA_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_VMA_H

#include <linux/tracepoint.h>

TRACE_EVENT(max_vma_count_exceeded,

	TP_PROTO(struct task_struct *task),

	TP_ARGS(task),

	TP_STRUCT__entry(
		__string(comm,	task->comm)
		__field(pid_t,	tgid)
	),

	TP_fast_assign(
		__assign_str(comm);
		__entry->tgid = task->tgid;
	),

	TP_printk("comm=%s tgid=%d", __get_str(comm), __entry->tgid)
);

#endif /*  _TRACE_VMA_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
