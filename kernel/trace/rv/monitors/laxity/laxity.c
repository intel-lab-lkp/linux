// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "laxity"

#include <trace/events/sched.h>
#include <rv_trace.h>

#define RV_MON_TYPE RV_MON_PER_OBJ
#define HA_TIMER_TYPE HA_TIMER_WHEEL
/* The start condition is on server_stop, allocation likely fails on PREEMPT_RT */
#define DA_SKIP_AUTO_ALLOC
typedef struct sched_dl_entity *monitor_target;
#include "laxity.h"
#include <rv/ha_monitor.h>
#include <monitors/deadline/deadline.h>

/* allow replenish when running only right after server start */
#define REPLENISH_NS TICK_NSEC

static inline u64 period_ns(struct ha_monitor *ha_mon)
{
	return ha_get_target(ha_mon)->dl_period + TICK_NSEC;
}

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_laxity env, u64 time_ns)
{
	if (env == clk_laxity)
		return ha_get_clk_ns(ha_mon, env, time_ns);
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_laxity env, u64 time_ns)
{
	if (env == clk_laxity)
		ha_reset_clk_ns(ha_mon, env, time_ns);
}

static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state == idle_wait_laxity)
		return ha_check_invariant_ns(ha_mon, clk_laxity, time_ns);
	else if (curr_state == replenish_wait_laxity)
		return ha_check_invariant_ns(ha_mon, clk_laxity, time_ns);
	else if (curr_state == zero_laxity_wait_laxity)
		return ha_check_invariant_ns(ha_mon, clk_laxity, time_ns);
	return true;
}

static inline void ha_convert_inv_guard(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state == next_state)
		return;
	if (curr_state == zero_laxity_wait_laxity)
		ha_inv_to_guard(ha_mon, clk_laxity, period_ns(ha_mon), time_ns);
}

static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	bool res = true;

	if (curr_state == stopped_laxity && event == dl_server_start_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == stopped_laxity && event == dl_server_start_running_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == idle_wait_laxity && event == dl_replenish_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == idle_wait_laxity && event == dl_replenish_idle_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == replenish_wait_laxity && event == dl_replenish_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == replenish_wait_laxity && event == dl_replenish_idle_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == replenish_wait_laxity && event == dl_replenish_running_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == running_laxity && event == dl_replenish_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == running_laxity && event == dl_replenish_idle_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == running_laxity && event == dl_replenish_running_laxity)
		res = ha_monitor_env_invalid(ha_mon, clk_laxity) ||
		      ha_get_env(ha_mon, clk_laxity, time_ns) < REPLENISH_NS;
	else if (curr_state == running_laxity && event == dl_throttle_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == zero_laxity_wait_laxity && event == dl_replenish_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == zero_laxity_wait_laxity && event == dl_replenish_idle_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	else if (curr_state == zero_laxity_wait_laxity && event == dl_replenish_running_laxity)
		ha_reset_env(ha_mon, clk_laxity, time_ns);
	return res;
}

static inline void ha_setup_invariants(struct ha_monitor *ha_mon,
				       enum states curr_state, enum events event,
				       enum states next_state, u64 time_ns)
{
	if (next_state == curr_state && event != dl_replenish_laxity &&
	    event != dl_replenish_idle_laxity)
		return;
	if (next_state == idle_wait_laxity)
		ha_start_timer_ns(ha_mon, clk_laxity, period_ns(ha_mon), time_ns);
	else if (next_state == replenish_wait_laxity)
		ha_start_timer_ns(ha_mon, clk_laxity, period_ns(ha_mon), time_ns);
	else if (next_state == zero_laxity_wait_laxity)
		ha_start_timer_ns(ha_mon, clk_laxity, period_ns(ha_mon), time_ns);
	else if (curr_state == idle_wait_laxity)
		ha_cancel_timer(ha_mon);
	else if (curr_state == replenish_wait_laxity)
		ha_cancel_timer(ha_mon);
	else if (curr_state == zero_laxity_wait_laxity)
		ha_cancel_timer(ha_mon);
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	if (!ha_verify_invariants(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	ha_convert_inv_guard(ha_mon, curr_state, event, next_state, time_ns);

	if (!ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	ha_setup_invariants(ha_mon, curr_state, event, next_state, time_ns);

	return true;
}

static void handle_dl_replenish(void *data, struct sched_dl_entity *dl_se,
				int cpu, uint8_t type)
{
	if (!is_server_type(type))
		return;
	/* Special replenish happening after throttle, ignore it */
	if (dl_se->dl_defer_running && dl_se->dl_throttled)
		return;
	if (dl_se->dl_defer_running)
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_replenish_running_laxity);
	else if (idle_cpu(cpu))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_replenish_idle_laxity);
	else
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_replenish_laxity);
}

static void handle_dl_server_start(void *data, struct sched_dl_entity *dl_se,
				   int cpu, uint8_t type)
{
	if (!is_server_type(type))
		return;
	if (dl_se->dl_defer_running)
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_server_start_running_laxity);
	else
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_server_start_laxity);
}

static void handle_dl_server_stop(void *data, struct sched_dl_entity *dl_se,
				  int cpu, uint8_t type)
{
	if (is_server_type(type))
		da_handle_start_event(EXPAND_ID(dl_se, cpu, type), dl_server_stop_laxity);
}

static void handle_dl_throttle(void *data, struct sched_dl_entity *dl_se,
			       int cpu, uint8_t type)
{
	if (is_server_type(type))
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_throttle_laxity);
}

static void handle_dl_update(void *data, struct sched_dl_entity *dl_se,
			       int cpu, uint8_t type)
{
	if (!is_server_type(type) || idle_cpu(cpu) || dl_se->dl_defer_running)
		return;
	/* The idle flag can be cleared without passing from an actual replenish */
	da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_update_laxity);
}


static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	if (!next->dl_server)
		return;
	da_handle_event(EXPAND_ID(next->dl_server, task_cpu(next),
				  get_server_type(next)),
			sched_switch_in_laxity);
}

static void handle_sched_enqueue(void *data, struct task_struct *tsk, int cpu)
{
	struct sched_dl_entity *dl_se = NULL;
	uint8_t type = get_server_type(tsk);

	if (is_server_type(type))
		dl_se = get_server(tsk, type);
	if (dl_se)
		da_handle_event(EXPAND_ID(dl_se, cpu, type), dl_server_resume_laxity);
}

static int enable_laxity(void)
{
	int retval;

	retval = da_monitor_init();
	if (retval)
		return retval;

	retval = init_storage(true);
	if (retval)
		return retval;
	rv_attach_trace_probe("laxity", sched_dl_replenish_tp, handle_dl_replenish);
	rv_attach_trace_probe("laxity", sched_dl_server_start_tp, handle_dl_server_start);
	rv_attach_trace_probe("laxity", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_attach_trace_probe("laxity", sched_dl_throttle_tp, handle_dl_throttle);
	rv_attach_trace_probe("laxity", sched_dl_update_tp, handle_dl_update);
	rv_attach_trace_probe("laxity", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("laxity", sched_enqueue_tp, handle_sched_enqueue);

	return 0;
}

static void disable_laxity(void)
{
	rv_this.enabled = 0;

	rv_detach_trace_probe("laxity", sched_dl_replenish_tp, handle_dl_replenish);
	rv_detach_trace_probe("laxity", sched_dl_server_start_tp, handle_dl_server_start);
	rv_detach_trace_probe("laxity", sched_dl_server_stop_tp, handle_dl_server_stop);
	rv_detach_trace_probe("laxity", sched_dl_throttle_tp, handle_dl_throttle);
	rv_detach_trace_probe("laxity", sched_dl_update_tp, handle_dl_update);
	rv_detach_trace_probe("laxity", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("laxity", sched_enqueue_tp, handle_sched_enqueue);

	da_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "laxity",
	.description = "deferrable servers wait for zero-laxity and run.",
	.enable = enable_laxity,
	.disable = disable_laxity,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_laxity(void)
{
	return rv_register_monitor(&rv_this, &rv_deadline);
}

static void __exit unregister_laxity(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_laxity);
module_exit(unregister_laxity);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("laxity: deferrable servers wait for zero-laxity and run.");
