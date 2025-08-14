// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "nomiss"

#include <uapi/linux/sched/types.h>
#include <trace/events/syscalls.h>
#include <trace/events/sched.h>
#include <trace/events/task.h>
#include <rv_trace.h>
#include <monitors/deadline/deadline.h>

#define RV_MON_TYPE RV_MON_PER_OBJ
/* The start condition is on sched_switch, it's dangerous to allocate there */
#define DA_SKIP_AUTO_ALLOC
typedef struct sched_dl_entity *monitor_target;
#include "nomiss.h"
#include <rv/ha_monitor.h>

/*
 * da_get_id - Get the id from a dl server
 *
 * Deadline tasks use the task's PID, while fair servers use the negated cpu.
 */
static inline da_id_type da_get_id(monitor_target target)
{
	if (target->dl_server)
		return get_server_id();
	return container_of(target, struct task_struct, dl)->pid;
}

/*
 * User configurable deadline threshold. If the total utilisation of deadline
 * tasks is larger than 1, they are only guaranteed bounded tardiness. See
 * Documentation/scheduler/sched-deadline.rst for more details.
 */
static u64 deadline_thresh = 0;
module_param(deadline_thresh, ullong, 0644);
#define DEADLINE_LEFT_NS(ha_mon) (ha_get_target(ha_mon)->deadline + deadline_thresh)

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_nomiss env)
{
	if (env == clk_nomiss)
		return ha_get_clk_ns(ha_mon, env);
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_nomiss env)
{
	if (env == clk_nomiss)
		ha_reset_clk_ns(ha_mon, env);
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state)
{
	bool res = true;

	if (curr_state == sleeping_nomiss && event == sched_switch_in_nomiss)
		ha_reset_env(ha_mon, clk_nomiss);
	else if (curr_state == throttled_nomiss && event == sched_switch_in_nomiss)
		ha_reset_env(ha_mon, clk_nomiss);

	if (next_state == curr_state || !res)
		return res;
	if (next_state == running_nomiss)
		ha_start_timer_ns(ha_mon, clk_nomiss, DEADLINE_LEFT_NS(ha_mon));
	else if (curr_state == running_nomiss)
		res = !ha_cancel_timer(ha_mon);
	return res;
}

static void handle_dl_throttle(void *data, struct sched_dl_entity *dl)
{
	da_handle_event(dl, dl_throttle_nomiss);
}

static void handle_dl_server_start(void *data, struct sched_dl_entity *dl)
{
	da_handle_start_event(dl, sched_switch_in_nomiss);
}

static void handle_dl_server_stop(void *data, struct sched_dl_entity *dl, bool hard)
{
	if (hard)
		da_handle_event(dl, sched_switch_suspend_nomiss);
}

static void handle_sched_switch(void *data, bool preempt, struct task_struct *prev, struct task_struct *next, unsigned int prev_state)
{
	if (prev_state != TASK_RUNNING && prev->policy == SCHED_DEADLINE)
		da_handle_event(&prev->dl, sched_switch_suspend_nomiss);
	if (next->policy == SCHED_DEADLINE)
		da_handle_start_event(&next->dl, sched_switch_in_nomiss);
}

static void handle_syscall(void *data, struct pt_regs *regs, long id)
{
	struct task_struct *p;
	int new_policy = -1;

	new_policy = extract_params(regs, id, &p);
	if (new_policy < 0 || new_policy == p->policy)
		return;
	if (p->policy == SCHED_DEADLINE)
		da_reset(&p->dl);
	else if (new_policy == SCHED_DEADLINE)
		da_create_conditional(&p->dl);
}

static void handle_sched_wakeup(void *data, struct task_struct *tsk)
{
	if (tsk->policy == SCHED_DEADLINE)
		da_handle_start_event(&tsk->dl, sched_wakeup_nomiss);
}

static void handle_newtask(void *data, struct task_struct *task, unsigned long flags)
{
	/* Might be superfluous as tasks are not started with this policy.. */
	if (task->policy == SCHED_DEADLINE)
		da_create_storage(&task->dl, NULL);
}

static void handle_exit(void *data, struct task_struct *p, bool group_dead)
{
	if (p->policy == SCHED_DEADLINE)
		da_destroy_storage(&p->dl);
}

/*
 * Initialise monitors for all tasks and pre-allocate the storage for servers.
 * This is necessary since we don't have access to the servers here and
 * allocation can cause deadlocks from their tracepoints. We can only fill
 * pre-initialised storage from there.
 */
static inline int init_storage(void)
{
	struct task_struct *g, *p;
	int cpu;

	for_each_possible_cpu(cpu) {
		/* The servers' ids are determined according to da_get_id */
		if (!da_create_empty_storage(-cpu))
			goto fail;
	}

	for_each_process_thread(g, p) {
		if (p->policy == SCHED_DEADLINE) {
			if (!da_create_storage(&p->dl, NULL))
				goto fail;
		}
	}
	return 0;

fail:
	da_monitor_destroy();
	return -ENOMEM;
}

static int enable_nomiss(void)
{
	int retval;

	retval = da_monitor_init();
	if (retval)
		return retval;

	retval = init_storage();
	if (retval)
		return retval;
	rv_attach_trace_probe("nomiss", sched_dl_throttle_tp, handle_dl_throttle);
	rv_attach_trace_probe("nomiss", sched_dl_server_start_tp, handle_dl_server_start);
	rv_attach_trace_probe("nomiss", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_attach_trace_probe("nomiss", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("nomiss", sched_wakeup, handle_sched_wakeup);
	if (!should_skip_syscall_handle())
		rv_attach_trace_probe("nomiss", sys_enter, handle_syscall);
	rv_attach_trace_probe("nomiss", task_newtask, handle_newtask);
	rv_attach_trace_probe("nomiss", sched_process_exit, handle_exit);

	return 0;
}

static void disable_nomiss(void)
{
	rv_nomiss.enabled = 0;

	/* Those are RCU writers, detach earlier hoping to close a bit faster */
	rv_detach_trace_probe("nomiss", task_newtask, handle_newtask);
	rv_detach_trace_probe("nomiss", sched_process_exit, handle_exit);
	if (!should_skip_syscall_handle())
		rv_detach_trace_probe("nomiss", sys_enter, handle_syscall);

	rv_detach_trace_probe("nomiss", sched_dl_throttle_tp, handle_dl_throttle);
	rv_detach_trace_probe("nomiss", sched_dl_server_start_tp, handle_dl_server_start);
	rv_detach_trace_probe("nomiss", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_detach_trace_probe("nomiss", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("nomiss", sched_wakeup, handle_sched_wakeup);

	da_monitor_destroy();
}

static struct rv_monitor rv_nomiss = {
	.name = "nomiss",
	.description = "dl entities run to completion before their deadiline.",
	.enable = enable_nomiss,
	.disable = disable_nomiss,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_nomiss(void)
{
	return rv_register_monitor(&rv_nomiss, &rv_deadline);
}

static void __exit unregister_nomiss(void)
{
	rv_unregister_monitor(&rv_nomiss);
}

module_init(register_nomiss);
module_exit(unregister_nomiss);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("nomiss: dl entities run to completion before their deadiline.");
