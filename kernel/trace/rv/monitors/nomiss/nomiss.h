/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of nomiss automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME nomiss

enum states_nomiss {
	running_nomiss = 0,
	sleeping_nomiss,
	throttled_nomiss,
	state_max_nomiss
};

#define INVALID_STATE state_max_nomiss

enum events_nomiss {
	dl_throttle_nomiss = 0,
	sched_switch_in_nomiss,
	sched_switch_suspend_nomiss,
	sched_wakeup_nomiss,
	event_max_nomiss
};

enum envs_nomiss {
	clk_nomiss = 0,
	env_max_nomiss,
	env_max_stored_nomiss = env_max_nomiss
};

_Static_assert(env_max_stored_nomiss <= MAX_HA_ENV_LEN, "Not enough slots");

struct automaton_nomiss {
	char *state_names[state_max_nomiss];
	char *event_names[event_max_nomiss];
	char *env_names[env_max_nomiss];
	unsigned char function[state_max_nomiss][event_max_nomiss];
	unsigned char initial_state;
	bool final_states[state_max_nomiss];
};

static const struct automaton_nomiss automaton_nomiss = {
	.state_names = {
		"running",
		"sleeping",
		"throttled"
	},
	.event_names = {
		"dl_throttle",
		"sched_switch_in",
		"sched_switch_suspend",
		"sched_wakeup"
	},
	.env_names = {
		"clk"
	},
	.function = {
		{
			throttled_nomiss,
			running_nomiss,
			sleeping_nomiss,
			running_nomiss
		},
		{
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			running_nomiss
		},
		{
			throttled_nomiss,
			running_nomiss,
			throttled_nomiss,
			running_nomiss
		},
	},
	.initial_state = running_nomiss,
	.final_states = { 1, 0, 0 },
};
