// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <rv/instrumentation.h>
#include <rv/da_monitor.h>

#define MODULE_NAME "srs"

#include <trace/events/sched.h>
#include <rv_trace.h>
#include <monitors/sched/sched.h>

#include "srs.h"

static struct rv_monitor rv_srs;
DECLARE_DA_MON_PER_TASK(srs, unsigned char);

static void handle_sched_need_resched(void *data, struct task_struct *tsk, int cpu, int tif)
{
	if (tif == TIF_NEED_RESCHED)
		da_handle_event_srs(tsk, sched_need_resched_srs);
	else
		da_handle_event_srs(tsk, sched_need_resched_lazy_srs);
}

static void handle_schedule_entry(void *data, bool preempt, unsigned long ip)
{
	/* special case from preempt_enable */
	if (preempt && unlikely(!tif_need_resched() && test_preempt_need_resched()))
		da_handle_event_srs(current, sched_need_resched_srs);
}

static void handle_sched_set_state(void *data, struct task_struct *tsk, int state)
{
	if (state == TASK_RUNNING)
		da_handle_event_srs(tsk, sched_set_state_runnable_srs);
	else
		da_handle_event_srs(tsk, sched_set_state_sleepable_srs);
}

static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	if (preempt)
		da_handle_event_srs(prev, sched_switch_preempt_srs);
	else if (prev_state == TASK_RUNNING)
		da_handle_start_run_event_srs(prev, sched_switch_yield_srs);
	else if (prev_state == TASK_RTLOCK_WAIT)
		/* special case of sleeping task with racy conditions */
		da_handle_event_srs(prev, sched_switch_blocking_srs);
	else
		da_handle_event_srs(prev, sched_switch_suspend_srs);
	/* switch in also leads to sleepable or rescheduling */
	if (task_is_running(next) && !test_tsk_thread_flag(next, TIF_NEED_RESCHED))
		da_handle_start_event_srs(next, sched_switch_in_srs);
	else
		da_handle_event_srs(next, sched_switch_in_srs);
}

static void handle_sched_switch_vain(void *data, bool preempt,
				     struct task_struct *tsk,
				     unsigned int tsk_state)
{
	da_handle_event_srs(tsk, sched_switch_vain_srs);
}

static void handle_sched_wakeup(void *data, struct task_struct *p)
{
	da_handle_event_srs(p, sched_wakeup_srs);
}

static int enable_srs(void)
{
	int retval;

	retval = da_monitor_init_srs();
	if (retval)
		return retval;

	rv_attach_trace_probe("srs", sched_set_need_resched_tp, handle_sched_need_resched);
	rv_attach_trace_probe("srs", sched_set_state_tp, handle_sched_set_state);
	rv_attach_trace_probe("srs", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("srs", sched_switch_vain_tp, handle_sched_switch_vain);
	rv_attach_trace_probe("srs", sched_wakeup, handle_sched_wakeup);
	rv_attach_trace_probe("srs", sched_entry_tp, handle_schedule_entry);

	return 0;
}

static void disable_srs(void)
{
	rv_srs.enabled = 0;

	rv_detach_trace_probe("srs", sched_set_need_resched_tp, handle_sched_need_resched);
	rv_detach_trace_probe("srs", sched_set_state_tp, handle_sched_set_state);
	rv_detach_trace_probe("srs", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("srs", sched_switch_vain_tp, handle_sched_switch_vain);
	rv_detach_trace_probe("srs", sched_wakeup, handle_sched_wakeup);
	rv_detach_trace_probe("srs", sched_entry_tp, handle_schedule_entry);

	da_monitor_destroy_srs();
}

static struct rv_monitor rv_srs = {
	.name = "srs",
	.description = "switch after resched or sleep.",
	.enable = enable_srs,
	.disable = disable_srs,
	.reset = da_monitor_reset_all_srs,
	.enabled = 0,
};

static int __init register_srs(void)
{
	rv_register_monitor(&rv_srs, &rv_sched);
	return 0;
}

static void __exit unregister_srs(void)
{
	rv_unregister_monitor(&rv_srs);
}

module_init(register_srs);
module_exit(unregister_srs);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("srs: switch after resched or sleep.");
