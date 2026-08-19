/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RV_TLOB_H
#define _RV_TLOB_H

/*
 * C representation of the tlob hybrid automaton (see tlob.dot).
 *
 * States: stopped (initial; parked), running (on CPU), waiting (runqueue),
 * sleeping (blocked).  Events: start/stop (tlob_start_task/tlob_stop_task),
 * sleep/preempt/wakeup/switch_in (sched tracepoints).
 *
 * "stop" fires only from running (both callers run on CPU); "stopped"
 * leaves only via "start" (fresh start or in-place restart).  running[start]
 * is INVALID: a stray re-start must not silently reset the budget clock.
 *
 * Invariant: clk_elapsed < BUDGET_NS() in running/waiting/sleeping; stopped
 * parks the window, no clock while parked.  start re-inits the monitor
 * (da_handle_start_run_event()); stop dispatches after ha_cancel_timer_sync();
 * final teardown uses ha_cancel_timer_sync() + da_monitor_reset() +
 * da_destroy_storage().
 *
 * Format: Documentation/trace/rv/deterministic_automata.rst
 */

#include <linux/rv.h>
#include <linux/sched.h>

#define MONITOR_NAME tlob

enum states_tlob {
	stopped_tlob,
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
	stop_tlob,
	switch_in_tlob,
	wakeup_tlob,
	event_max_tlob,
};

/*
 * HA clock env: clk_elapsed, wall-clock since the window start; anchored in
 * running/waiting/sleeping, cleared on stop.
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
		"stopped",
		"running",
		"sleeping",
		"waiting",
	},
	.event_names = {
		"preempt",
		"sleep",
		"start",
		"stop",
		"switch_in",
		"wakeup",
	},
	.env_names = {
		"clk_elapsed",
	},
	.function = {
		/* stopped (initial; window parked, sched events not routed) */
		{
			INVALID_STATE,	/* preempt   (not on CPU)                    */
			INVALID_STATE,	/* sleep     (not on CPU)                    */
			running_tlob,	/* start     (tlob_start_task, fresh or restart) */
			INVALID_STATE,	/* stop      (already stopped)                */
			INVALID_STATE,	/* switch_in (not on CPU)                    */
			INVALID_STATE,	/* wakeup    (not on CPU)                    */
		},
		/* running */
		{
			waiting_tlob,	/* preempt   (sched_switch, prev_state == 0) */
			sleeping_tlob,	/* sleep     (sched_switch, prev_state != 0) */
			INVALID_STATE,	/* start     (running task's START is -EALREADY) */
			stopped_tlob,	/* stop      (tlob_stop_task)                */
			INVALID_STATE,	/* switch_in (already on CPU)                */
			INVALID_STATE,	/* wakeup    (TASK_RUNNING can't be woken)   */
		},
		/* sleeping */
		{
			INVALID_STATE,	/* preempt   (not on CPU)                    */
			INVALID_STATE,	/* sleep     (already sleeping)              */
			INVALID_STATE,	/* start     (not in running state)          */
			INVALID_STATE,	/* stop      (not in running state)          */
			INVALID_STATE,	/* switch_in (must go through waiting first) */
			waiting_tlob,	/* wakeup                                    */
		},
		/* waiting */
		{
			INVALID_STATE,	/* preempt   (not on CPU)                    */
			INVALID_STATE,	/* sleep     (not on CPU)                    */
			INVALID_STATE,	/* start     (not in running state)          */
			INVALID_STATE,	/* stop      (not in running state)          */
			running_tlob,	/* switch_in                                 */
			INVALID_STATE,	/* wakeup    (already TASK_RUNNING)          */
		},
	},
	.initial_state = stopped_tlob,
	.final_states = { 0, 1, 0, 0 },
};

/*
 * Hard cap on concurrently monitored tasks.  tlob_ws_pool pre-allocates
 * this many slots; a fresh start past the cap returns -ENOSPC with bounded
 * latency (mempool_alloc_preallocated() never touches the allocator).
 * Restarts reuse the same slot.
 */
#define TLOB_MAX_MONITORED	64U

/* Maximum binary path length for uprobe binding. */
#define TLOB_MAX_PATH		256

/* Minimum monitoring budget (1 us). */
#define TLOB_MIN_THRESHOLD_NS	1000ULL

/* Upper budget bound (1 hour): keeps the u64 ns accumulators far from overflow. */
#define TLOB_MAX_THRESHOLD_NS	3600000000000ULL

#if IS_ENABLED(CONFIG_TLOB_KUNIT_TEST)
int tlob_parse_uprobe_line(char *buf, u64 *thr_out, char **path_out,
			   loff_t *start_out, loff_t *stop_out);
int tlob_parse_remove_line(char *buf, char **path_out, loff_t *start_out);
#endif /* CONFIG_TLOB_KUNIT_TEST */

#endif /* _RV_TLOB_H */
