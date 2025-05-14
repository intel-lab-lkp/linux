// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <rv/instrumentation.h>
#include <rv/da_monitor.h>

#define MODULE_NAME "nrp"

#include <trace/events/sched.h>
#include <rv_trace.h>
#include <monitors/sched/sched.h>

#include "nrp.h"

static struct rv_monitor rv_nrp;
DECLARE_DA_MON_PER_TASK(nrp, unsigned char);

static void handle_sched_need_resched(void *data, struct task_struct *tsk,
				      int cpu, int tif)
{
	da_handle_event_nrp(tsk, sched_need_resched_nrp);
}

static void handle_schedule_entry(void *data, bool preempt, unsigned long ip)
{
	/*
	 * In theory, a preemption can only occur after the need_resched flag
	 * is set. In practice, however, we may see a preemption where the flag
	 * is not set. This can happen in one specific condition:
	 *
	 * need_resched
	 *		preempt_schedule()
	 *					preempt_schedule_irq()
	 *						__schedule()
	 * !need_resched
	 *			__schedule()
	 *
	 * In the situation above, we start a standard preemption (e.g. from
	 * preempt_enable when the flag is set), an interrupts occurs before we
	 * schedule and, on its exit path, it schedules, which clears the
	 * need_resched flag.
	 * When the preempted task runs again, we continue the standard
	 * preemption started earlier, although the flag is no longer set.
	 *
	 * The following workaround allows the model not to fail in this
	 * condition, but makes it weaker. In fact, we are not proving that:
	 *  1. we don't miss any event setting need_resched
	 *  2. we don't preempt when not required
	 *
	 *  Ideally, we should find a way to narrow down the condition, however
	 *  that's rather tricky without adding several tracepoints in
	 *  undesired locations.
	 */
	if (preempt && unlikely(!tif_need_resched()))
		da_handle_event_nrp(current, sched_need_resched_nrp);
}

static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	if (preempt)
		da_handle_start_event_nrp(prev, sched_switch_preempt_nrp);
	else
		da_handle_start_event_nrp(prev, sched_switch_other_nrp);
}

static void handle_sched_switch_vain(void *data, bool preempt,
				     struct task_struct *tsk,
				     unsigned int tsk_state)
{
	if (preempt)
		da_handle_start_event_nrp(tsk, sched_switch_vain_preempt_nrp);
	else
		da_handle_start_event_nrp(tsk, sched_switch_vain_nrp);
}

static int enable_nrp(void)
{
	int retval;

	retval = da_monitor_init_nrp();
	if (retval)
		return retval;

	rv_attach_trace_probe("snep", sched_entry_tp, handle_schedule_entry);
	rv_attach_trace_probe("nrp", sched_set_need_resched_tp, handle_sched_need_resched);
	rv_attach_trace_probe("nrp", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("nrp", sched_switch_vain_tp, handle_sched_switch_vain);

	return 0;
}

static void disable_nrp(void)
{
	rv_nrp.enabled = 0;

	rv_detach_trace_probe("snep", sched_entry_tp, handle_schedule_entry);
	rv_detach_trace_probe("nrp", sched_set_need_resched_tp, handle_sched_need_resched);
	rv_detach_trace_probe("nrp", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("nrp", sched_switch_vain_tp, handle_sched_switch_vain);

	da_monitor_destroy_nrp();
}

static struct rv_monitor rv_nrp = {
	.name = "nrp",
	.description = "need resched preempts.",
	.enable = enable_nrp,
	.disable = disable_nrp,
	.reset = da_monitor_reset_all_nrp,
	.enabled = 0,
};

static int __init register_nrp(void)
{
	return rv_register_monitor(&rv_nrp, &rv_sched);
}

static void __exit unregister_nrp(void)
{
	rv_unregister_monitor(&rv_nrp);
}

module_init(register_nrp);
module_exit(unregister_nrp);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("nrp: need resched preempts.");
