/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RV_TLOB_H
#define _RV_TLOB_H

/*
 * C representation of the tlob hybrid automaton (see tlob.dot).
 * "stop" fires only from running (both callers run on CPU); running x
 * start is INVALID: a stray re-start must not reset the budget clock.
 * Invariant: clk_elapsed <= BUDGET_NS() in running/waiting/sleeping;
 * stopped is exempt.
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
 * HA clock env: clk_elapsed, elapsed time since the window start; anchored in
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
		/* stopped: sched events self-loop */
		{
			stopped_tlob,	/* preempt   */
			stopped_tlob,	/* sleep     */
			running_tlob,	/* start     */
			INVALID_STATE,	/* stop      */
			stopped_tlob,	/* switch_in */
			stopped_tlob,	/* wakeup    */
		},
		/* running */
		{
			waiting_tlob,	/* preempt (prev_state == 0)  */
			sleeping_tlob,	/* sleep (prev_state != 0)    */
			INVALID_STATE,	/* start (is -EALREADY)       */
			stopped_tlob,	/* stop                       */
			INVALID_STATE,	/* switch_in                  */
			running_tlob,	/* wakeup                     */
		},
		/* sleeping */
		{
			INVALID_STATE,	/* preempt   */
			INVALID_STATE,	/* sleep     */
			INVALID_STATE,	/* start     */
			INVALID_STATE,	/* stop      */
			INVALID_STATE,	/* switch_in (via waiting) */
			waiting_tlob,	/* wakeup    */
		},
		/* waiting */
		{
			INVALID_STATE,	/* preempt   */
			INVALID_STATE,	/* sleep     */
			INVALID_STATE,	/* start     */
			INVALID_STATE,	/* stop      */
			running_tlob,	/* switch_in */
			waiting_tlob,	/* wakeup    */
		},
	},
	.initial_state = stopped_tlob,
	.final_states = { 1, 0, 0, 0 },
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

/* Maximum binding name length (excluding NUL). */
#define TLOB_MAX_NAME		32

/* Minimum monitoring budget (1 us). */
#define TLOB_MIN_THRESHOLD_NS	1000ULL

/* Upper budget bound (1 hour). */
#define TLOB_MAX_THRESHOLD_NS	3600000000000ULL

#endif /* _RV_TLOB_H */
