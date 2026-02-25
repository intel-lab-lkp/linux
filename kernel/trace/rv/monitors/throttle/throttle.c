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

#define RV_MON_TYPE RV_MON_PER_OBJ
#define HA_TIMER_TYPE HA_TIMER_WHEEL
/* The start condition is on sched_switch, it's dangerous to allocate there */
#define DA_SKIP_AUTO_ALLOC
typedef struct sched_dl_entity *monitor_target;
#include "throttle.h"
#include <rv/ha_monitor.h>
#include <monitors/deadline/deadline.h>

#define THROTTLED_TIME_NS TICK_NSEC
/* with sched_feat(HRTICK_DL) the threshold can be lower */
#define RUNTIME_THRESH TICK_NSEC
/*
 * On systems with CPU frequency scaling or turbo boost, deadline tasks can run
 * longer than their runtime as this is scaled according to the frequency. As a
 * result, this constraint cannot work.
 */
static bool skip_runtime_check;
module_param(skip_runtime_check, bool, 0644);

static inline u64 runtime_left_ns(struct ha_monitor *ha_mon)
{
	return ha_get_target(ha_mon)->runtime + RUNTIME_THRESH;
}

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_throttle env, u64 time_ns)
{
	if (env == clk_throttle)
		return ha_get_clk_ns(ha_mon, env, time_ns);
	else if (env == is_constr_dl_throttle)
		return !dl_is_implicit(ha_get_target(ha_mon));
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_throttle env, u64 time_ns)
{
	if (env == clk_throttle)
		ha_reset_clk_ns(ha_mon, env, time_ns);
}

static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state == running_throttle && !skip_runtime_check)
		return ha_check_invariant_ns(ha_mon, clk_throttle, time_ns);
	else if (curr_state == throttled_throttle)
		return ha_check_invariant_ns(ha_mon, clk_throttle, time_ns);
	return true;
}

static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	bool res = true;

	if (curr_state == running_throttle && event == dl_replenish_throttle)
		ha_reset_env(ha_mon, clk_throttle, time_ns);
	else if (curr_state == running_throttle && event == dl_throttle_throttle)
		ha_reset_env(ha_mon, clk_throttle, time_ns);
	else if (curr_state == armed_throttle && event == sched_switch_in_throttle)
		ha_reset_env(ha_mon, clk_throttle, time_ns);
	else if (curr_state == preempted_throttle && event == dl_throttle_throttle)
		res = ha_get_env(ha_mon, is_constr_dl_throttle, time_ns) == 1ull;
	else if (curr_state == preempted_throttle && event == sched_switch_in_throttle)
		ha_reset_env(ha_mon, clk_throttle, time_ns);
	else if (curr_state == throttled_throttle && event == dl_replenish_throttle)
		ha_reset_env(ha_mon, clk_throttle, time_ns);
	return res;
}

static inline void ha_setup_invariants(struct ha_monitor *ha_mon,
				       enum states curr_state, enum events event,
				       enum states next_state, u64 time_ns)
{
	if (next_state == curr_state && event != dl_replenish_throttle)
		return;
	if (next_state == running_throttle && !skip_runtime_check)
		ha_start_timer_ns(ha_mon, clk_throttle, runtime_left_ns(ha_mon), time_ns);
	else if (next_state == throttled_throttle)
		ha_start_timer_ns(ha_mon, clk_throttle, THROTTLED_TIME_NS, time_ns);
	else if (curr_state == running_throttle)
		ha_cancel_timer(ha_mon);
	else if (curr_state == throttled_throttle)
		ha_cancel_timer(ha_mon);
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	if (!ha_verify_invariants(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	if (!ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	ha_setup_invariants(ha_mon, curr_state, event, next_state, time_ns);

	return true;
}

static void handle_dl_replenish(void *data, struct sched_dl_entity *dl_se,
				int cpu, uint8_t type)
{
	if (is_supported_type(type))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_replenish_throttle);
}

static void handle_dl_throttle(void *data, struct sched_dl_entity *dl_se,
			       int cpu, uint8_t type)
{
	if (is_supported_type(type))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_throttle_throttle);
}

static void handle_dl_server_stop(void *data, struct sched_dl_entity *dl_se,
				  int cpu, uint8_t type)
{
	if (is_supported_type(type))
		da_handle_start_run_event(EXPAND_ID(dl_se, cpu, type), sched_switch_out_throttle);
}

static inline void handle_server_switch(struct task_struct *next, int cpu, u8 type)
{
	struct sched_dl_entity *dl_se = get_server(next, type);

	if (!dl_se)
		return;
	if (get_server_type(next) == type || is_idle_task(next))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_defer_arm_throttle);
	else
		da_handle_event(EXPAND_ID(dl_se, cpu, type), sched_switch_out_throttle);
}

static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	int cpu = task_cpu(next);

	if (prev->policy == SCHED_DEADLINE)
		da_handle_event(EXPAND_ID_TASK(prev), sched_switch_out_throttle);
	if (next->policy == SCHED_DEADLINE)
		da_handle_start_event(EXPAND_ID_TASK(next), sched_switch_in_throttle);

	/*
	 * The server is available in next only if the next task is boosted,
	 * otherwise we need to retrieve it.
	 * Here the server continues in the state running/armed until actually
	 * stopped, this works since we continue expecting a throttle.
	 */
	if (next->dl_server) {
		da_handle_start_event(EXPAND_ID(next->dl_server, cpu,
						get_server_type(next)),
				      sched_switch_in_throttle);
	} else {
		handle_server_switch(next, cpu, DL_SERVER_FAIR);
		if (IS_ENABLED(CONFIG_SCHED_CLASS_EXT))
			handle_server_switch(next, cpu, DL_SERVER_EXT);
	}
}

static void handle_sched_enqueue(void *data, struct task_struct *tsk, int cpu)
{
	struct sched_dl_entity *dl_se = NULL;
	uint8_t type = get_server_type(tsk);

	if (is_server_type(type))
		dl_se = get_server(tsk, type);
	/*
	 * An enqueue is counted as server arming only in case of a change in
	 * scheduler where the task is moved to another scheduler's runqueue.
	 */
	if (dl_se && task_is_running(tsk) && sched_task_on_rq(tsk))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_defer_arm_throttle);
}

static void handle_sys_enter(void *data, struct pt_regs *regs, long id)
{
	struct task_struct *p;
	int new_policy = -1;

	new_policy = extract_params(regs, id, &p);
	if (new_policy < 0 || new_policy == p->policy)
		return;
	if (p->policy == SCHED_DEADLINE)
		da_reset(EXPAND_ID_TASK(p));
	else if (new_policy == SCHED_DEADLINE)
		da_create_or_get(EXPAND_ID_TASK(p));
}

static int enable_throttle(void)
{
	int retval;

	retval = da_monitor_init();
	if (retval)
		return retval;

	retval = init_storage(false);
	if (retval)
		return retval;
	rv_attach_trace_probe("throttle", sched_dl_replenish_tp, handle_dl_replenish);
	rv_attach_trace_probe("throttle", sched_dl_throttle_tp, handle_dl_throttle);
	rv_attach_trace_probe("throttle", sched_enqueue_tp, handle_sched_enqueue);
	rv_attach_trace_probe("throttle", sched_switch, handle_sched_switch);
	if (!should_skip_syscall_handle())
		rv_attach_trace_probe("throttle", sys_enter, handle_sys_enter);
	rv_attach_trace_probe("throttle", task_newtask, handle_newtask);
	rv_attach_trace_probe("throttle", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_attach_trace_probe("throttle", sched_process_exit, handle_exit);

	return 0;
}

static void disable_throttle(void)
{
	rv_this.enabled = 0;

	/* Those are RCU writers, detach earlier hoping to close a bit faster */
	rv_detach_trace_probe("throttle", task_newtask, handle_newtask);
	rv_detach_trace_probe("throttle", sched_process_exit, handle_exit);
	if (!should_skip_syscall_handle())
		rv_detach_trace_probe("throttle", sys_enter, handle_sys_enter);

	rv_detach_trace_probe("throttle", sched_dl_replenish_tp, handle_dl_replenish);
	rv_detach_trace_probe("throttle", sched_dl_throttle_tp, handle_dl_throttle);
	rv_detach_trace_probe("throttle", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_detach_trace_probe("throttle", sched_enqueue_tp, handle_sched_enqueue);
	rv_detach_trace_probe("throttle", sched_switch, handle_sched_switch);

	da_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "throttle",
	.description = "throttle dl entities when they use up their runtime.",
	.enable = enable_throttle,
	.disable = disable_throttle,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_throttle(void)
{
	return rv_register_monitor(&rv_this, &rv_deadline);
}

static void __exit unregister_throttle(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_throttle);
module_exit(unregister_throttle);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("throttle: throttle dl entities when they use up their runtime.");
