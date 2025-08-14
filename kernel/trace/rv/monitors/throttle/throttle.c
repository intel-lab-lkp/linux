// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "throttle"

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
#include "throttle.h"
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

/* with sched_feat(HRTICK_DL) the threshold should be lower */
#define RUNTIME_THRESH jiffies_to_nsecs(1)

static inline u64 runtime_left_ns(struct ha_monitor *ha_mon)
{
	return ha_get_target(ha_mon)->runtime + RUNTIME_THRESH;
}

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_throttle env)
{
	if (env == clk_throttle)
		return ha_get_clk_ns(ha_mon, env);
	else if (env == yielded_throttle)
		return ha_get_target(ha_mon)->dl_yielded;
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_throttle env)
{
	if (env == clk_throttle)
		ha_reset_clk_ns(ha_mon, env);
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state)
{
	bool res = true;

	if (curr_state == armed_throttle && event == sched_switch_in_throttle)
		ha_reset_env(ha_mon, clk_throttle);
	else if (curr_state == armed_throttled_throttle && event == dl_throttle_throttle)
		res = ha_get_env(ha_mon, yielded_throttle) == 1ull;
	else if (curr_state == preempted_throttle && event == sched_switch_in_throttle)
		ha_reset_env(ha_mon, clk_throttle);
	else if (curr_state == running_throttle && event == dl_replenish_throttle)
		ha_reset_env(ha_mon, clk_throttle);
	else if (curr_state == throttled_throttle && event == dl_replenish_throttle)
		ha_reset_env(ha_mon, clk_throttle);

	if ((next_state == curr_state && event != dl_replenish_throttle) || !res)
		return res;
	if (next_state == running_throttle)
		ha_start_timer_ns(ha_mon, clk_throttle, runtime_left_ns(ha_mon));
	else if (curr_state == running_throttle)
		res = !ha_cancel_timer(ha_mon);
	return res;
}

static void handle_dl_replenish(void *data, struct sched_dl_entity *dl)
{
	da_handle_event(dl, dl_replenish_throttle);
}

static void handle_dl_throttle(void *data, struct sched_dl_entity *dl)
{
	da_handle_event(dl, dl_throttle_throttle);
}

static inline struct sched_dl_entity *get_fair_server(struct task_struct *tsk)
{
	if (tsk->dl_server)
		return tsk->dl_server;
	return da_get_target_by_id(get_server_id());
}

static void handle_sched_switch(void *data, bool preempt, struct task_struct *prev, struct task_struct *next, unsigned int prev_state)
{
	struct sched_dl_entity *dl;

	if (prev->policy == SCHED_DEADLINE)
		da_handle_event(&prev->dl, sched_switch_out_throttle);
	if (next->policy == SCHED_DEADLINE)
		da_handle_start_event(&next->dl, sched_switch_in_throttle);

	/*
	 * The server is available in next only if the next task is boosted,
	 * otherwise we need to retrieve it.
	 */
	dl = get_fair_server(next);
	if (!dl)
		return;
	if (next->dl_server)
		da_handle_start_event(next->dl_server, sched_switch_in_throttle);
	else if (is_idle_task(next) || next->policy == SCHED_NORMAL)
		da_handle_event(dl, dl_defer_arm_throttle);
	else
		da_handle_event(dl, sched_switch_out_throttle);
}

static void handle_syscall(void *data, struct pt_regs *regs, long id)
{
	struct task_struct *p;
	int new_policy = -1;

	new_policy = extract_params(regs, id, &p);
	if (new_policy < 0 || new_policy == p->policy)
		return;
	if (p->policy == SCHED_DEADLINE) {
		da_reset(&p->dl);
		/*
		 * When a task changes from SCHED_DEADLINE to SCHED_NORMAL, the
		 * runtime after the change is counted in the fair server.
		 */
		if (new_policy == SCHED_NORMAL) {
			struct sched_dl_entity *dl = get_fair_server(p);
			if (!dl)
				return;
			da_handle_event(dl, dl_defer_arm_throttle);
		}
	} else if (new_policy == SCHED_DEADLINE) {
		da_create_conditional(&p->dl);
	}
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

static int enable_throttle(void)
{
	int retval;

	retval = da_monitor_init();
	if (retval)
		return retval;

	retval = init_storage();
	if (retval)
		return retval;
	rv_attach_trace_probe("throttle", sched_dl_replenish_tp, handle_dl_replenish);
	rv_attach_trace_probe("throttle", sched_dl_throttle_tp, handle_dl_throttle);
	rv_attach_trace_probe("throttle", sched_switch, handle_sched_switch);
	if (!should_skip_syscall_handle())
		rv_attach_trace_probe("throttle", sys_enter, handle_syscall);
	rv_attach_trace_probe("throttle", task_newtask, handle_newtask);
	rv_attach_trace_probe("throttle", sched_process_exit, handle_exit);

	return 0;
}

static void disable_throttle(void)
{
	rv_throttle.enabled = 0;

	/* Those are RCU writers, detach earlier hoping to close a bit faster */
	rv_detach_trace_probe("throttle", task_newtask, handle_newtask);
	rv_detach_trace_probe("throttle", sched_process_exit, handle_exit);
	if (!should_skip_syscall_handle())
		rv_detach_trace_probe("throttle", sys_enter, handle_syscall);

	rv_detach_trace_probe("throttle", sched_dl_replenish_tp, handle_dl_replenish);
	rv_detach_trace_probe("throttle", sched_dl_throttle_tp, handle_dl_throttle);
	rv_detach_trace_probe("throttle", sched_switch, handle_sched_switch);

	da_monitor_destroy();
}

static struct rv_monitor rv_throttle = {
	.name = "throttle",
	.description = "throttle dl entities when they use up their runtime.",
	.enable = enable_throttle,
	.disable = disable_throttle,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_throttle(void)
{
	return rv_register_monitor(&rv_throttle, &rv_deadline);
}

static void __exit unregister_throttle(void)
{
	rv_unregister_monitor(&rv_throttle);
}

module_init(register_throttle);
module_exit(unregister_throttle);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("throttle: throttle dl entities when they use up their runtime.");
