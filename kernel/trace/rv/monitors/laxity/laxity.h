/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of laxity automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME laxity

enum states_laxity {
	stopped_laxity,
	idle_wait_laxity,
	replenish_wait_laxity,
	running_laxity,
	zero_laxity_wait_laxity,
	state_max_laxity,
};

#define INVALID_STATE state_max_laxity

enum events_laxity {
	dl_replenish_laxity,
	dl_replenish_idle_laxity,
	dl_replenish_running_laxity,
	dl_server_resume_laxity,
	dl_server_start_laxity,
	dl_server_start_running_laxity,
	dl_server_stop_laxity,
	dl_throttle_laxity,
	dl_update_laxity,
	sched_switch_in_laxity,
	event_max_laxity,
};

enum envs_laxity {
	clk_laxity,
	env_max_laxity,
	env_max_stored_laxity = env_max_laxity,
};

_Static_assert(env_max_stored_laxity <= MAX_HA_ENV_LEN, "Not enough slots");
#define HA_CLK_NS

struct automaton_laxity {
	char *state_names[state_max_laxity];
	char *event_names[event_max_laxity];
	char *env_names[env_max_laxity];
	unsigned char function[state_max_laxity][event_max_laxity];
	unsigned char initial_state;
	bool final_states[state_max_laxity];
};

static const struct automaton_laxity automaton_laxity = {
	.state_names = {
		"stopped",
		"idle_wait",
		"replenish_wait",
		"running",
		"zero_laxity_wait",
	},
	.event_names = {
		"dl_replenish",
		"dl_replenish_idle",
		"dl_replenish_running",
		"dl_server_resume",
		"dl_server_start",
		"dl_server_start_running",
		"dl_server_stop",
		"dl_throttle",
		"dl_update",
		"sched_switch_in",
	},
	.env_names = {
		"clk",
	},
	.function = {
		{
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			stopped_laxity,
			zero_laxity_wait_laxity,
			running_laxity,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
		},
		{
			zero_laxity_wait_laxity,
			idle_wait_laxity,
			INVALID_STATE,
			zero_laxity_wait_laxity,
			INVALID_STATE,
			INVALID_STATE,
			stopped_laxity,
			INVALID_STATE,
			zero_laxity_wait_laxity,
			INVALID_STATE,
		},
		{
			zero_laxity_wait_laxity,
			idle_wait_laxity,
			running_laxity,
			replenish_wait_laxity,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			zero_laxity_wait_laxity,
			INVALID_STATE,
		},
		{
			zero_laxity_wait_laxity,
			zero_laxity_wait_laxity,
			running_laxity,
			running_laxity,
			INVALID_STATE,
			INVALID_STATE,
			stopped_laxity,
			replenish_wait_laxity,
			zero_laxity_wait_laxity,
			running_laxity,
		},
		{
			zero_laxity_wait_laxity,
			idle_wait_laxity,
			running_laxity,
			zero_laxity_wait_laxity,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			zero_laxity_wait_laxity,
			INVALID_STATE,
		},
	},
	.initial_state = stopped_laxity,
	.final_states = { 1, 0, 0, 0, 0 },
};
