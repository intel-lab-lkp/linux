// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched/rt.h>
#include <linux/preempt.h>
#include <linux/rv.h>

#include <uapi/linux/futex.h>
#include <trace/events/syscalls.h>
#include <trace/events/sched.h>
#include <trace/events/task.h>
#include <trace/events/lock.h>
#include <trace/events/preemptirq.h>

#include <rv_trace.h>
#include <rv/instrumentation.h>


#include "ba.h"

struct rtapp_block_data {
	struct task_struct *woken_task;
	struct task_struct *stopping_task;
};

static void handle_sys_enter(void *data, struct pt_regs *regs, long id)
{
	unsigned long args[6];
	int op, cmd;

	switch (id) {
	case __NR_nanosleep:
	case __NR_clock_nanosleep:
#ifdef __NR_clock_nanosleep_time64
	case __NR_clock_nanosleep_time64:
#endif
		rv_rtapp_block_atom_update(current, DO_NANOSLEEP, true);
		break;

	case __NR_futex:
#ifdef __NR_futex_time64
	case __NR_futex_time64:
#endif
		syscall_get_arguments(current, regs, args);
		op = args[1];
		cmd = op & FUTEX_CMD_MASK;

		if (cmd == FUTEX_LOCK_PI || cmd == FUTEX_LOCK_PI2)
			rv_rtapp_block_atom_update(current, FUTEX_LOCK_WITH_PI, true);
		break;
	}
}

static void handle_sys_exit(void *data, struct pt_regs *regs, long ret)
{
	struct ltl_monitor *mon = rv_rtapp_block_get_monitor(current);

	rv_rtapp_block_atom_set(mon, FUTEX_LOCK_WITH_PI, false);
	rv_rtapp_block_atom_update(current, DO_NANOSLEEP, false);
}

static void handle_sched_switch(void *data, bool preempt, struct task_struct *prev,
				struct task_struct *next, unsigned int prev_state)
{
	if (prev_state & TASK_INTERRUPTIBLE)
		rv_rtapp_block_atom_update(prev, SLEEP, true);
	rv_rtapp_block_atom_update(next, SLEEP, false);
}

void rv_rtapp_block_atoms_fetch(struct task_struct *task, struct ltl_monitor *mon)
{
	rv_rtapp_block_atom_set(mon, RT, rt_task(task));
	rv_rtapp_block_atom_set(mon, USER_TASK, !(task->flags & PF_KTHREAD));
}

void rv_rtapp_block_atoms_init(struct task_struct *task, struct ltl_monitor *mon)
{
	rv_rtapp_block_atom_set(mon, SLEEP, false);
	rv_rtapp_block_atom_set(mon, DO_NANOSLEEP, false);
	rv_rtapp_block_atom_set(mon, FUTEX_LOCK_PI, false);
	rv_rtapp_block_atom_set(mon, WAKEUP_RT_TASK, false);
	rv_rtapp_block_atom_set(mon, RT_MUTEX_WAKING_WAITER, false);
	rv_rtapp_block_atom_set(mon, STOPPING_WOKEN_TASK, false);
	rv_rtapp_block_atom_set(mon, WOKEN_TASK_IS_MIGRATION, false);
	rv_rtapp_block_atom_set(mon, WOKEN_TASK_IS_RCU, false);
}

static void handle_rt_mutex_wake_waiter_begin(void *, struct task_struct *task)
{
	rv_rtapp_block_atom_update(task, RT_MUTEX_WAKING_WAITER, true);
}

static void handle_rt_mutex_wake_waiter_end(void *, struct task_struct *task)
{
	rv_rtapp_block_atom_update(task, RT_MUTEX_WAKING_WAITER, false);
}

static void handle_sched_kthread_stop(void *, struct task_struct *task)
{
	struct ltl_monitor *mon = rv_rtapp_block_get_monitor(current);
	struct rtapp_block_data *data = rv_rtapp_block_get_data(mon);

	data->stopping_task = task;
}

static void handle_sched_kthread_stop_ret(void *, int)
{
	struct ltl_monitor *mon = rv_rtapp_block_get_monitor(current);
	struct rtapp_block_data *data = rv_rtapp_block_get_data(mon);

	data->stopping_task = NULL;
}

static void handle_sched_wakeup(void *, struct task_struct *task)
{
	struct ltl_monitor *mon = rv_rtapp_block_get_monitor(current);
	struct rtapp_block_data *data = rv_rtapp_block_get_data(mon);

	if (!in_task())
		return;

	if (this_cpu_read(hardirq_context))
		return;

	if (!rt_task(task))
		return;

	data->woken_task = task;

	if (!strncmp(task->comm, "migration/", strlen("migration/")))
		rv_rtapp_block_atom_set(mon, WOKEN_TASK_IS_MIGRATION, true);
	if (!strcmp(task->comm, "rcu_preempt"))
		rv_rtapp_block_atom_set(mon, WOKEN_TASK_IS_RCU, true);
	if (data->stopping_task == data->woken_task)
		rv_rtapp_block_atom_set(mon, STOPPING_WOKEN_TASK, true);

	rv_rtapp_block_atom_update(current, WAKEUP_RT_TASK, true);

	rv_rtapp_block_atom_set(mon, WOKEN_TASK_IS_MIGRATION, false);
	rv_rtapp_block_atom_set(mon, WOKEN_TASK_IS_RCU, false);
	rv_rtapp_block_atom_set(mon, STOPPING_WOKEN_TASK, false);
	rv_rtapp_block_atom_update(current, WAKEUP_RT_TASK, false);
}

static int enable_rtapp_block(void)
{
	int ret;

	ret = rv_rtapp_block_init(sizeof(struct rtapp_block_data));

	if (ret)
		return ret;

	rv_attach_trace_probe("rtapp_block", sched_wakeup, handle_sched_wakeup);
	rv_attach_trace_probe("rtapp_block", rt_mutex_wake_waiter_begin,
					     handle_rt_mutex_wake_waiter_begin);
	rv_attach_trace_probe("rtapp_block", rt_mutex_wake_waiter_end,
					     handle_rt_mutex_wake_waiter_end);
	rv_attach_trace_probe("rtapp_block", sched_kthread_stop, handle_sched_kthread_stop);
	rv_attach_trace_probe("rtapp_block", sched_kthread_stop_ret, handle_sched_kthread_stop_ret);
	rv_attach_trace_probe("rtapp_block", sys_enter, handle_sys_enter);
	rv_attach_trace_probe("rtapp_block", sys_exit, handle_sys_exit);
	rv_attach_trace_probe("rtapp_block", sched_switch, handle_sched_switch);

	return 0;
}

static void disable_rtapp_block(void)
{
	rv_detach_trace_probe("rtapp_block", sched_wakeup, handle_sched_wakeup);
	rv_detach_trace_probe("rtapp_block", rt_mutex_wake_waiter_begin,
					     handle_rt_mutex_wake_waiter_begin);
	rv_detach_trace_probe("rtapp_block", rt_mutex_wake_waiter_end,
					     handle_rt_mutex_wake_waiter_end);
	rv_detach_trace_probe("rtapp_block", sched_kthread_stop, handle_sched_kthread_stop);
	rv_detach_trace_probe("rtapp_block", sched_kthread_stop_ret, handle_sched_kthread_stop_ret);
	rv_detach_trace_probe("rtapp_block", sys_enter, handle_sys_enter);
	rv_detach_trace_probe("rtapp_block", sys_exit, handle_sys_exit);
	rv_detach_trace_probe("rtapp_block", sched_switch, handle_sched_switch);

	rv_rtapp_block_destroy();
}

static struct rv_monitor rv_rtapp_block = {
	.name = "rtapp_block",
	.description = "Monitor that RT tasks are not blocked by non-RT tasks",
	.enable = enable_rtapp_block,
	.disable = disable_rtapp_block,
};

void rv_rtapp_block_error(struct task_struct *task, struct ltl_monitor *mon)
{
	struct rtapp_block_data *data = rv_rtapp_block_get_data(mon);
	struct task_struct *woken = data->woken_task;

	bool sleep = rv_rtapp_block_atom_get(mon, SLEEP);

	if (sleep)
		trace_rtapp_block_sleep_error(task);
	else
		trace_rtapp_block_wakeup_error(task, woken);

#ifdef CONFIG_RV_REACTORS
	if (!rv_rtapp_block.react)
		return;

	if (sleep) {
		rv_rtapp_block.react("rv: %s[%d](RT) is blocked\n", task->comm, task->pid);
	} else {
		rv_rtapp_block.react("rv: %s[%d](RT) was blocked %s[%d](non-RT)\n",
					woken->comm, woken->pid,
					task->comm, task->pid);
	}
#endif
}

static int __init register_rtapp_block(void)
{
	rv_register_monitor(&rv_rtapp_block);
	return 0;
}

static void __exit unregister_rtapp_block(void)
{
	rv_unregister_monitor(&rv_rtapp_block);
}

module_init(register_rtapp_block);
module_exit(unregister_rtapp_block);
