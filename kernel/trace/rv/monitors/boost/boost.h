/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of boost automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME boost

enum states_boost {
	stopped_boost,
	idle_boost,
	ready_boost,
	running_boost,
	throttled_boost,
	throttled_running_boost,
	state_max_boost,
};

#define INVALID_STATE state_max_boost

enum events_boost {
	dl_replenish_boost,
	dl_server_idle_boost,
	dl_server_resume_boost,
	dl_server_resume_throttled_boost,
	dl_server_start_boost,
	dl_server_stop_boost,
	dl_throttle_boost,
	sched_switch_in_boost,
	sched_switch_out_boost,
	event_max_boost,
};

enum envs_boost {
	clk_boost,
	env_max_boost,
	env_max_stored_boost = env_max_boost,
};

_Static_assert(env_max_stored_boost <= MAX_HA_ENV_LEN, "Not enough slots");
#define HA_CLK_NS

struct automaton_boost {
	char *state_names[state_max_boost];
	char *event_names[event_max_boost];
	char *env_names[env_max_boost];
	unsigned char function[state_max_boost][event_max_boost];
	unsigned char initial_state;
	bool final_states[state_max_boost];
};

static const struct automaton_boost automaton_boost = {
	.state_names = {
		"stopped",
		"idle",
		"ready",
		"running",
		"throttled",
		"throttled_running",
	},
	.event_names = {
		"dl_replenish",
		"dl_server_idle",
		"dl_server_resume",
		"dl_server_resume_throttled",
		"dl_server_start",
		"dl_server_stop",
		"dl_throttle",
		"sched_switch_in",
		"sched_switch_out",
	},
	.env_names = {
		"clk",
	},
	.function = {
		{
			INVALID_STATE,
			stopped_boost,
			stopped_boost,
			stopped_boost,
			ready_boost,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			stopped_boost,
		},
		{
			idle_boost,
			idle_boost,
			ready_boost,
			throttled_boost,
			INVALID_STATE,
			stopped_boost,
			idle_boost,
			INVALID_STATE,
			INVALID_STATE,
		},
		{
			ready_boost,
			idle_boost,
			ready_boost,
			ready_boost,
			INVALID_STATE,
			stopped_boost,
			throttled_boost,
			running_boost,
			ready_boost,
		},
		{
			running_boost,
			idle_boost,
			running_boost,
			running_boost,
			INVALID_STATE,
			stopped_boost,
			throttled_running_boost,
			INVALID_STATE,
			ready_boost,
		},
		{
			ready_boost,
			idle_boost,
			INVALID_STATE,
			throttled_boost,
			INVALID_STATE,
			stopped_boost,
			throttled_boost,
			throttled_running_boost,
			INVALID_STATE,
		},
		{
			running_boost,
			idle_boost,
			INVALID_STATE,
			throttled_running_boost,
			INVALID_STATE,
			INVALID_STATE,
			throttled_running_boost,
			INVALID_STATE,
			throttled_boost,
		},
	},
	.initial_state = stopped_boost,
	.final_states = { 1, 0, 0, 0, 0, 0 },
};
