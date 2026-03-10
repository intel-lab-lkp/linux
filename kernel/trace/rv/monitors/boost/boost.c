// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "boost"

#include <trace/events/sched.h>
#include <rv_trace.h>

#define RV_MON_TYPE RV_MON_PER_OBJ
#define DA_SKIP_AUTO_ALLOC
#define HA_TIMER_TYPE HA_TIMER_WHEEL
typedef struct sched_dl_entity *monitor_target;
#include "boost.h"
#include <rv/ha_monitor.h>
#include <monitors/deadline/deadline.h>

static inline u64 server_threshold_ns(struct ha_monitor *ha_mon)
{
	struct sched_dl_entity *dl_se = ha_get_target(ha_mon);

	return dl_se->dl_deadline + TICK_NSEC - dl_se->runtime;
}

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_boost env, u64 time_ns)
{
	if (env == clk_boost)
		return ha_get_clk_ns(ha_mon, env, time_ns);
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_boost env, u64 time_ns)
{
	if (env == clk_boost)
		ha_reset_clk_ns(ha_mon, env, time_ns);
}

static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state == ready_boost)
		return ha_check_invariant_ns(ha_mon, clk_boost, time_ns);
	return true;
}

static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	bool res = true;

	if (curr_state == stopped_boost && event == dl_server_start_boost)
		ha_reset_env(ha_mon, clk_boost, time_ns);
	else if (curr_state == idle_boost && event == dl_replenish_boost)
		ha_reset_env(ha_mon, clk_boost, time_ns);
	else if (curr_state == ready_boost && event == dl_replenish_boost)
		ha_reset_env(ha_mon, clk_boost, time_ns);
	else if (curr_state == running_boost && event == dl_replenish_boost)
		ha_reset_env(ha_mon, clk_boost, time_ns);
	else if (curr_state == throttled_boost && event == dl_replenish_boost)
		ha_reset_env(ha_mon, clk_boost, time_ns);
	else if (curr_state == throttled_running_boost && event == dl_replenish_boost)
		ha_reset_env(ha_mon, clk_boost, time_ns);
	return res;
}

static inline void ha_setup_invariants(struct ha_monitor *ha_mon,
				       enum states curr_state, enum events event,
				       enum states next_state, u64 time_ns)
{
	if (next_state == curr_state && event != dl_replenish_boost)
		return;
	if (next_state == ready_boost)
		ha_start_timer_ns(ha_mon, clk_boost, server_threshold_ns(ha_mon), time_ns);
	else if (curr_state == ready_boost)
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
	if (is_server_type(type))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_replenish_boost);
}

static inline void handle_server_switch(struct task_struct *next,
					struct task_struct *prev, int cpu,
					u8 type)
{
	struct sched_dl_entity *dl_se = get_server(next, type);

	if (!dl_se)
		return;
	if (is_idle_task(next))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_server_idle_boost);
	else if (get_server_type(next) == type && !rt_or_dl_task(next))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), sched_switch_in_boost);
	else if (get_server_type(prev) == type && !is_idle_task(prev))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), sched_switch_out_boost);
}

static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	int cpu = task_cpu(next);

	/*
	 * The server is available in next only if the next task is boosted,
	 * otherwise we need to retrieve it.
	 * This monitor considers switch in/out whenever a task related to the
	 * server (i.e. fair) is scheduled in or out, boosted or not.
	 * Any switch to the same policy is ignored.
	 * PI boosted tasks are not considered fair.
	 */
	if (get_server_type(next) == get_server_type(prev) &&
	    !is_idle_task(next) && !is_idle_task(prev))
		return;
	handle_server_switch(next, prev, cpu, DL_SERVER_FAIR);
	if (IS_ENABLED(CONFIG_SCHED_CLASS_EXT))
		handle_server_switch(next, prev, cpu, DL_SERVER_EXT);
}

static void handle_sched_enqueue(void *data, struct task_struct *tsk, int cpu)
{
	struct sched_dl_entity *dl_se = NULL;
	uint8_t type = get_server_type(tsk);

	if (is_server_type(type))
		dl_se = get_server(tsk, type);
	if (dl_se) {
		da_handle_event(EXPAND_ID(dl_se, cpu, type),
				dl_se->runtime > 0 ?
					dl_server_resume_boost :
					dl_server_resume_throttled_boost);
	}
}

static void handle_sched_dequeue(void *data, struct task_struct *tsk, int cpu)
{
	struct sched_dl_entity *dl_se = NULL;
	uint8_t type = get_server_type(tsk);

	if (is_server_type(type))
		dl_se = get_server(tsk, type);
	/*
	 * A dequeue is counted as switching out only in case of a change in
	 * scheduler where the task is moved to another scheduler's runqueue.
	 */
	if (dl_se && task_is_running(tsk) && sched_task_on_rq(tsk))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), sched_switch_out_boost);
}

static void handle_dl_server_start(void *data, struct sched_dl_entity *dl_se,
				   int cpu, uint8_t type)
{
	if (is_server_type(type))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_server_start_boost);
}

static void handle_dl_server_stop(void *data, struct sched_dl_entity *dl_se,
				  int cpu, uint8_t type)
{
	if (is_server_type(type))
		da_handle_start_event(EXPAND_ID(dl_se, cpu, type), dl_server_stop_boost);
}

static void handle_dl_throttle(void *data, struct sched_dl_entity *dl_se,
			       int cpu, uint8_t type)
{
	if (is_server_type(type))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_throttle_boost);
}

static int enable_boost(void)
{
	int retval;

	retval = da_monitor_init();
	if (retval)
		return retval;

	retval = init_storage(true);
	if (retval)
		return retval;
	rv_attach_trace_probe("boost", sched_dl_replenish_tp, handle_dl_replenish);
	rv_attach_trace_probe("boost", sched_dl_server_start_tp, handle_dl_server_start);
	rv_attach_trace_probe("boost", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_attach_trace_probe("boost", sched_dl_throttle_tp, handle_dl_throttle);
	rv_attach_trace_probe("boost", sched_enqueue_tp, handle_sched_enqueue);
	rv_attach_trace_probe("boost", sched_dequeue_tp, handle_sched_dequeue);
	rv_attach_trace_probe("boost", sched_switch, handle_sched_switch);

	return 0;
}

static void disable_boost(void)
{
	rv_this.enabled = 0;

	rv_detach_trace_probe("boost", sched_dl_replenish_tp, handle_dl_replenish);
	rv_detach_trace_probe("boost", sched_dl_server_start_tp, handle_dl_server_start);
	rv_detach_trace_probe("boost", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_detach_trace_probe("boost", sched_dl_throttle_tp, handle_dl_throttle);
	rv_detach_trace_probe("boost", sched_enqueue_tp, handle_sched_enqueue);
	rv_detach_trace_probe("boost", sched_dequeue_tp, handle_sched_dequeue);
	rv_detach_trace_probe("boost", sched_switch, handle_sched_switch);

	da_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "boost",
	.description = "fair tasks run either independently or boosted.",
	.enable = enable_boost,
	.disable = disable_boost,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_boost(void)
{
	return rv_register_monitor(&rv_this, &rv_deadline);
}

static void __exit unregister_boost(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_boost);
module_exit(unregister_boost);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("boost: fair tasks run either independently or boosted.");
