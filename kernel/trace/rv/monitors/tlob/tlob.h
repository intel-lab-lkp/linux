/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RV_TLOB_H
#define _RV_TLOB_H

/*
 * C representation of the tlob hybrid automaton.
 *
 * Three-state HA following sched_stat / wwnr monitor naming conventions:
 *
 *   running  (initial) - task is executing on CPU          [sched_stat: runtime]
 *   waiting             - task is in runqueue, awaiting CPU [sched_stat: wait   ]
 *   sleeping            - task is blocked, awaiting resource[sched_stat: sleep  ]
 *
 * Events (derived from sched_switch / sched_wakeup tracepoints):
 *   sleep     - sched_switch, prev_state != 0   running  → sleeping
 *   preempt   - sched_switch, prev_state == 0   running  → waiting
 *   wakeup    - sched_wakeup                    sleeping → waiting
 *   switch_in - sched_switch, next == task      waiting  → running
 *
 * One HA clock invariant:
 *   clk_elapsed < BUDGET_NS()  active in all states  (total latency budget)
 *
 * task_start and task_stop are NOT DA events:
 *   task_start calls da_handle_start_event() to set initial state, then
 *   ha_reset_clk_ns() + ha_start_timer_ns() to initialise the clock and arm
 *   the timer directly.
 *   task_stop calls hrtimer_cancel() + da_monitor_reset() directly.
 *
 * For the format description see:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#include <linux/rv.h>
#include <linux/sched.h>

#define MONITOR_NAME tlob

enum states_tlob {
	running_tlob,
	waiting_tlob,
	sleeping_tlob,
	state_max_tlob,
};

#define INVALID_STATE state_max_tlob

enum events_tlob {
	sleep_tlob,
	preempt_tlob,
	wakeup_tlob,
	switch_in_tlob,
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
		"waiting",
		"sleeping",
	},
	.event_names = {
		"sleep",
		"preempt",
		"wakeup",
		"switch_in",
	},
	.env_names = {
		"clk_elapsed",
	},
	.function = {
		/* running */
		{
			sleeping_tlob,	/* sleep     (sched_switch, prev_state != 0) */
			waiting_tlob,	/* preempt   (sched_switch, prev_state == 0) */
			INVALID_STATE,	/* wakeup    (TASK_RUNNING can't be woken)   */
			INVALID_STATE,	/* switch_in (already on CPU)                */
		},
		/* waiting */
		{
			INVALID_STATE,	/* sleep     (not on CPU)                    */
			INVALID_STATE,	/* preempt   (not on CPU)                    */
			INVALID_STATE,	/* wakeup    (already TASK_RUNNING)          */
			running_tlob,	/* switch_in                                 */
		},
		/* sleeping */
		{
			INVALID_STATE,	/* sleep     (already sleeping)              */
			INVALID_STATE,	/* preempt   (not on CPU)                    */
			waiting_tlob,	/* wakeup                                    */
			INVALID_STATE,	/* switch_in (must go through waiting first) */
		},
	},
	.initial_state = running_tlob,
	.final_states = { 1, 0, 0 },
};

/* Maximum number of concurrently monitored tasks. */
#define TLOB_MAX_MONITORED	64U

/* Maximum binary path length for uprobe binding. */
#define TLOB_MAX_PATH		256

/* Exported to ioctl/uprobe layers and KUnit */
int tlob_start_task(struct task_struct *task, u64 threshold_us);
int tlob_stop_task(struct task_struct *task);

#if IS_ENABLED(CONFIG_KUNIT)
int tlob_init_monitor(void);
void tlob_destroy_monitor(void);
int tlob_enable_hooks(void);
void tlob_disable_hooks(void);
int tlob_create_or_delete_uprobe(char *buf);
int tlob_num_monitored_read(void);

struct tlob_captured_event {
	int  id;
	char state[16];
	char event[16];
	char next_state[16];
	bool final_state;
};

struct tlob_captured_error_env {
	int  id;
	char state[16];
	char event[16];
	char env[64];
};

struct tlob_captured_detail {
	int  pid;
	u64  threshold_us;
	u64  running_ns;
	u64  waiting_ns;
	u64  sleeping_ns;
};

int  tlob_register_kunit_probes(void);
void tlob_unregister_kunit_probes(void);
int  tlob_event_count_read(void);
void tlob_event_count_reset(void);
int  tlob_error_env_count_read(void);
void tlob_error_env_count_reset(void);
const struct tlob_captured_event     *tlob_last_event_read(void);
const struct tlob_captured_error_env *tlob_last_error_env_read(void);
const struct tlob_captured_detail    *tlob_last_detail_read(void);
#endif /* CONFIG_KUNIT */

#endif /* _RV_TLOB_H */
