/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tracepoints for PSI automatic monitor
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM psi_monitor

#if !defined(_TRACE_PSI_MONITOR_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PSI_MONITOR_H

#include <linux/types.h>
#include <linux/tracepoint.h>

TRACE_EVENT(psi_monitor_top_task,

	TP_PROTO(pid_t pid, const char *comm,
		unsigned long cpu_ms,
		unsigned long rss_kb,
		unsigned long io_kb,
		u64 score),

	TP_ARGS(pid, comm, cpu_ms, rss_kb, io_kb, score),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__string(comm, comm)
		__field(unsigned long, cpu_ms)
		__field(unsigned long, rss_kb)
		__field(unsigned long, io_kb)
		__field(u64, score)
	),

	TP_fast_assign(
		__entry->pid = pid;
		__assign_str(comm);
		__entry->cpu_ms = cpu_ms;
		__entry->rss_kb = rss_kb;
		__entry->io_kb = io_kb;
		__entry->score = score;
	),

	TP_printk("pid=%d comm=%s cpu_ms=%lu rss_kb=%lu io_kb=%lu score=%llu",
		__entry->pid, __get_str(comm),
		__entry->cpu_ms, __entry->rss_kb,
		__entry->io_kb,
		(unsigned long long)__entry->score)
);

#endif /* _TRACE_PSI_MONITOR_H */

/* This must be outside the header guard */
#include <trace/define_trace.h>
