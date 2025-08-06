// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <linux/sched/deadline.h>
#include <linux/sched/rt.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "rts"

#include <trace/events/sched.h>
#include <rv_trace.h>
#include <monitors/rtapp/rtapp.h>

#include "rts.h"
#include <rv/ltl_monitor.h>

static DEFINE_PER_CPU(unsigned int, nr_queued);

static void ltl_atoms_fetch(unsigned int cpu, struct ltl_monitor *mon)
{
}

static void ltl_atoms_init(unsigned int cpu, struct ltl_monitor *mon,
			   bool target_creation)
{
	ltl_atom_set(mon, LTL_SCHED_SWITCH, false);
	ltl_atom_set(mon, LTL_SCHED_SWITCH_DL, false);
	ltl_atom_set(mon, LTL_SCHED_SWITCH_RT, false);

	/*
	 * This may not be accurate, there may be enqueued RT tasks. But that's
	 * okay, the worst we get is a false negative. It will be accurate as
	 * soon as the CPU no longer has any queued RT task.
	 */
	ltl_atom_set(mon, LTL_RT_TASK_ENQUEUED, false);
}

static void handle_enqueue_task(void *data, int cpu, struct task_struct *task)
{
	unsigned int *queued = per_cpu_ptr(&nr_queued, cpu);

	if (!rt_task(task))
		return;

	(*queued)++;
	ltl_atom_update(cpu, LTL_RT_TASK_ENQUEUED, true);
}

static void handle_dequeue_task(void *data, int cpu, struct task_struct *task)
{
	unsigned int *queued = per_cpu_ptr(&nr_queued, cpu);

	if (!rt_task(task))
		return;

	/*
	 * This may not be accurate for a short time after the monitor is
	 * enabled, because there may be enqueued RT tasks which are not counted
	 * torward nr_queued. But that's okay, the worst we get is a false
	 * negative. nr_queued will be accurate as soon as the CPU no longer has
	 * any queued RT task.
	 */
	if (*queued)
		(*queued)--;
	if (!*queued)
		ltl_atom_update(cpu, LTL_RT_TASK_ENQUEUED, false);
}

static void handle_sched_switch(void *data, bool preempt, struct task_struct *prev,
				struct task_struct *next, unsigned int prev_state)
{
	unsigned int cpu = smp_processor_id();
	struct ltl_monitor *mon = ltl_get_monitor(cpu);

	ltl_atom_set(mon, LTL_SCHED_SWITCH_RT, rt_task(next));
	ltl_atom_set(mon, LTL_SCHED_SWITCH_DL, dl_task(next));
	ltl_atom_update(cpu, LTL_SCHED_SWITCH, true);

	ltl_atom_set(mon, LTL_SCHED_SWITCH_RT, false);
	ltl_atom_set(mon, LTL_SCHED_SWITCH_DL, false);
	ltl_atom_update(cpu, LTL_SCHED_SWITCH, false);
}

static int enable_rts(void)
{
	unsigned int cpu;
	int retval;

	retval = ltl_monitor_init();
	if (retval)
		return retval;

	for_each_possible_cpu(cpu) {
		unsigned int *queued = per_cpu_ptr(&nr_queued, cpu);

		*queued = 0;
	}

	rv_attach_trace_probe("rts", dequeue_task_tp, handle_dequeue_task);
	rv_attach_trace_probe("rts", enqueue_task_tp, handle_enqueue_task);
	rv_attach_trace_probe("rts", sched_switch, handle_sched_switch);

	return 0;
}

static void disable_rts(void)
{
	rv_detach_trace_probe("rts", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("rts", enqueue_task_tp, handle_enqueue_task);
	rv_detach_trace_probe("rts", dequeue_task_tp, handle_dequeue_task);

	ltl_monitor_destroy();
}

/*
 * This is the monitor register section.
 */
static struct rv_monitor rv_rts = {
	.name = "rts",
	.description = "Validate that real-time tasks are scheduled before lower-priority tasks",
	.enable = enable_rts,
	.disable = disable_rts,
};

static int __init register_rts(void)
{
	return rv_register_monitor(&rv_rts, &rv_rtapp);
}

static void __exit unregister_rts(void)
{
	rv_unregister_monitor(&rv_rts);
}

module_init(register_rts);
module_exit(unregister_rts);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nam Cao <namcao@linutronix.de>");
MODULE_DESCRIPTION("rts: Validate that real-time tasks are scheduled before lower-priority tasks");
