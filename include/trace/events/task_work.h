/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM task_work

#if !defined(_TRACE_TASK_WORK_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TASK_WORK_H

#include <linux/tracepoint.h>
#include <linux/task_work.h>

TRACE_DEFINE_ENUM(TWA_NONE);
TRACE_DEFINE_ENUM(TWA_RESUME);
TRACE_DEFINE_ENUM(TWA_SIGNAL);
TRACE_DEFINE_ENUM(TWA_SIGNAL_NO_IPI);
TRACE_DEFINE_ENUM(TWA_NMI_CURRENT);

#define show_task_work_notify_mode(notify)			\
	__print_symbolic(notify,				\
		{ TWA_NONE,           "TWA_NONE" },		\
		{ TWA_RESUME,         "TWA_RESUME" },		\
		{ TWA_SIGNAL,         "TWA_SIGNAL" },		\
		{ TWA_SIGNAL_NO_IPI,  "TWA_SIGNAL_NO_IPI" },	\
		{ TWA_NMI_CURRENT,    "TWA_NMI_CURRENT" })

/*
 * task_work_add() is split into two events:
 *
 *   task_work:add_request - fires before the cmpxchg that enqueues
 *                           @work. Guaranteed to happen-before any
 *                           run_start has picked the @work.
 *   task_work:add_done    - fires after the cmpxchg loop terminates,
 *                           carrying the final ret value.
 */
TRACE_EVENT(task_work_add_request,

	TP_PROTO(struct task_struct *task,
		 struct callback_head *work,
		 task_work_func_t func,
		 enum task_work_notify_mode notify),

	TP_ARGS(task, work, func, notify),

	TP_STRUCT__entry(
		__field(pid_t,		pid)
		__array(char,		comm, TASK_COMM_LEN)
		__field(void *,		work)
		__field(void *,		func)
		__field(int,		notify)
	),

	TP_fast_assign(
		__entry->pid		= task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->work		= work;
		__entry->func		= func;
		__entry->notify		= notify;
	),

	TP_printk("target_comm=%s target_pid=%d work=%p func=%ps notify=%s",
		__entry->comm, __entry->pid, __entry->work,
		__entry->func, show_task_work_notify_mode(__entry->notify))
);

TRACE_EVENT(task_work_add_done,

	TP_PROTO(struct task_struct *task,
		 struct callback_head *work,
		 int ret),

	TP_ARGS(task, work, ret),

	TP_STRUCT__entry(
		__field(pid_t,		pid)
		__array(char,		comm, TASK_COMM_LEN)
		__field(void *,		work)
		__field(int,		ret)
	),

	TP_fast_assign(
		__entry->pid		= task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->work		= work;
		__entry->ret		= ret;
	),

	TP_printk("target_comm=%s target_pid=%d work=%p ret=%d",
		__entry->comm, __entry->pid, __entry->work, __entry->ret)
);

DECLARE_EVENT_CLASS(task_work_run_template,

	TP_PROTO(struct task_struct *task,
		 struct callback_head *work,
		 task_work_func_t func),

	TP_ARGS(task, work, func),

	TP_STRUCT__entry(
		__field(pid_t,		pid)
		__array(char,		comm, TASK_COMM_LEN)
		__field(void *,		work)
		__field(void *,		func)
	),

	TP_fast_assign(
		__entry->pid		= task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->work		= work;
		__entry->func		= func;
	),

	TP_printk("comm=%s pid=%d work=%p func=%ps",
		__entry->comm, __entry->pid, __entry->work, __entry->func)
);

DEFINE_EVENT(task_work_run_template, task_work_run_start,
	TP_PROTO(struct task_struct *task, struct callback_head *work,
		 task_work_func_t func),
	TP_ARGS(task, work, func));

DEFINE_EVENT(task_work_run_template, task_work_run_end,
	TP_PROTO(struct task_struct *task, struct callback_head *work,
		 task_work_func_t func),
	TP_ARGS(task, work, func));

#endif /* _TRACE_TASK_WORK_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
