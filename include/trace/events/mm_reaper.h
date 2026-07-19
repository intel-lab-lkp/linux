/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mm_reaper

#if !defined(_TRACE_MM_REAPER_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MM_REAPER_H

#include <linux/tracepoint.h>
#include <linux/sched.h>
#include <linux/topology.h>

TRACE_EVENT(mm_async_teardown_queue,

	TP_PROTO(struct mm_struct *mm, unsigned long rss),

	TP_ARGS(mm, rss),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(int, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(unsigned long, rss)
		__field(int, node)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->rss = rss;
		__entry->node = numa_node_id();
	),

	TP_printk("mm=%p pid=%d comm=%s rss=%lukB node=%d",
		__entry->mm,
		__entry->pid,
		__entry->comm,
		__entry->rss << (PAGE_SHIFT - 10),
		__entry->node
	)
);

TRACE_EVENT(mm_async_teardown_reap,

	TP_PROTO(struct mm_struct *mm, unsigned long charged_rss, unsigned long live_rss),

	TP_ARGS(mm, charged_rss, live_rss),

	TP_STRUCT__entry(
		__field(struct mm_struct *, mm)
		__field(unsigned long, charged_rss)
		__field(unsigned long, live_rss)
		__field(int, node)
	),

	TP_fast_assign(
		__entry->mm = mm;
		__entry->charged_rss = charged_rss;
		__entry->live_rss = live_rss;
		__entry->node = numa_node_id();
	),

	TP_printk("mm=%p charged_rss=%lukB live_rss=%lukB node=%d",
		__entry->mm,
		__entry->charged_rss << (PAGE_SHIFT - 10),
		__entry->live_rss << (PAGE_SHIFT - 10),
		__entry->node
	)
);

#endif /* _TRACE_MM_REAPER_H */

#include <trace/define_trace.h>
