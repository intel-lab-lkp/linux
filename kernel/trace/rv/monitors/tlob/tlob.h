/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RV_TLOB_H
#define _RV_TLOB_H

/*
 * C representation of the tlob hybrid automaton.
 *
 * Three-state HA following sched_stat / wwnr monitor naming conventions:
 *
 *   running  (initial) - task on CPU           [sched_stat: runtime]
 *   waiting             - task in runqueue     [sched_stat: wait   ]
 *   sleeping            - task blocked         [sched_stat: sleep  ]
 *
 * Events (derived from sched_switch / sched_wakeup tracepoints):
 *   start     - tlob_start_task()    running -> running (resets clock)
 *   sleep     - sched_switch, prev_state != 0   running  -> sleeping
 *   preempt   - sched_switch, prev_state == 0   running  -> waiting
 *   wakeup    - sched_wakeup                    sleeping -> waiting
 *   switch_in - sched_switch, next == task      waiting  -> running
 *
 * One HA clock invariant:
 *   clk_elapsed < BUDGET_NS()  active in all states  (total latency budget)
 *
 * tlob_start_task() uses da_handle_start_run_event(start_tlob) to initialise
 * the monitor: the DA framework sets the initial state and then processes the
 * start event, which resets clk_elapsed and arms the budget hrtimer via the
 * generated ha_setup_invariants().
 * tlob_stop_task() calls ha_cancel_timer_sync() + da_monitor_reset() directly.
 *
 * For the format description see:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#include <linux/rv.h>
#include <linux/sched.h>

#define MONITOR_NAME tlob

enum states_tlob {
	running_tlob,
	sleeping_tlob,
	waiting_tlob,
	state_max_tlob,
};

#define INVALID_STATE state_max_tlob

enum events_tlob {
	preempt_tlob,
	sleep_tlob,
	start_tlob,
	switch_in_tlob,
	wakeup_tlob,
	event_max_tlob,
};

/*
 * HA environment variable: clk_elapsed is the only clock.
 * It measures wall-clock time since task_start and is active in all states.
 */
enum envs_tlob {
	clk_elapsed_tlob,
	env_max_tlob,
	env_max_stored_tlob = env_max_tlob,
};

_Static_assert(env_max_stored_tlob <= MAX_HA_ENV_LEN, "Not enough slots");
#define HA_CLK_NS

struct automaton_tlob {
	char *state_names[state_max_tlob];
	char *event_names[event_max_tlob];
	char *env_names[env_max_tlob];
	unsigned char function[state_max_tlob][event_max_tlob];
	unsigned char initial_state;
	bool final_states[state_max_tlob];
};

static const struct automaton_tlob automaton_tlob = {
	.state_names = {
		"running",
		"sleeping",
		"waiting",
	},
	.event_names = {
		"preempt",
		"sleep",
		"start",
		"switch_in",
		"wakeup",
	},
	.env_names = {
		"clk_elapsed",
	},
	.function = {
		/* running */
		{
			waiting_tlob,	/* preempt   (sched_switch, prev_state == 0) */
			sleeping_tlob,	/* sleep     (sched_switch, prev_state != 0) */
			running_tlob,	/* start     (tlob_start_task, resets clock)  */
			INVALID_STATE,	/* switch_in (already on CPU)                */
			INVALID_STATE,	/* wakeup    (TASK_RUNNING can't be woken)   */
		},
		/* sleeping */
		{
			INVALID_STATE,	/* preempt   (not on CPU)                    */
			INVALID_STATE,	/* sleep     (already sleeping)              */
			INVALID_STATE,	/* start     (not in running state)          */
			INVALID_STATE,	/* switch_in (must go through waiting first) */
			waiting_tlob,	/* wakeup                                    */
		},
		/* waiting */
		{
			INVALID_STATE,	/* preempt   (not on CPU)                    */
			INVALID_STATE,	/* sleep     (not on CPU)                    */
			INVALID_STATE,	/* start     (not in running state)          */
			running_tlob,	/* switch_in                                 */
			INVALID_STATE,	/* wakeup    (already TASK_RUNNING)          */
		},
	},
	.initial_state = running_tlob,
	.final_states = { 1, 0, 0 },
};

/* Maximum number of concurrently monitored tasks. */
#define TLOB_MAX_MONITORED	64U

/* Maximum binary path length for uprobe binding. */
#define TLOB_MAX_PATH		256

/* Minimum monitoring budget (1 us). */
#define TLOB_MIN_THRESHOLD_NS	1000ULL

/*
 * Upper bound on the monitoring budget (1 hour = 3 600 000 000 000 ns).
 * The ns-resolution accumulators (running_ns, waiting_ns, sleeping_ns)
 * are u64; keeping the window below this limit ensures they stay well
 * clear of u64 overflow and covers every realistic latency-monitoring
 * use case.
 */
#define TLOB_MAX_THRESHOLD_NS	3600000000000ULL

/* Exported to uprobe layer and KUnit tests */
int tlob_start_task(struct task_struct *task, u64 threshold_ns);
int tlob_stop_task(struct task_struct *task);

#if IS_ENABLED(CONFIG_KUNIT)
int tlob_parse_uprobe_line(char *buf, u64 *thr_out, char **path_out,
			   loff_t *start_out, loff_t *stop_out);
int tlob_parse_remove_line(char *buf, char **path_out, loff_t *start_out);
#endif /* CONFIG_KUNIT */

#endif /* _RV_TLOB_H */
