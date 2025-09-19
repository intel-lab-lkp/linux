/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of throttle automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME throttle

enum states_throttle {
	running_throttle,
	armed_throttle,
	armed_throttled_throttle,
	preempted_throttle,
	preempted_throttled_throttle,
	throttled_throttle,
	state_max_throttle,
};

#define INVALID_STATE state_max_throttle

enum events_throttle {
	dl_defer_arm_throttle,
	dl_replenish_throttle,
	dl_throttle_throttle,
	sched_switch_in_throttle,
	sched_switch_out_throttle,
	event_max_throttle,
};

enum envs_throttle {
	clk_throttle,
	is_constr_dl_throttle,
	yielded_throttle,
	env_max_throttle,
	env_max_stored_throttle = is_constr_dl_throttle,
};

_Static_assert(env_max_stored_throttle <= MAX_HA_ENV_LEN, "Not enough slots");
#define HA_CLK_NS

struct automaton_throttle {
	char *state_names[state_max_throttle];
	char *event_names[event_max_throttle];
	char *env_names[env_max_throttle];
	unsigned char function[state_max_throttle][event_max_throttle];
	unsigned char initial_state;
	bool final_states[state_max_throttle];
};

static const struct automaton_throttle automaton_throttle = {
	.state_names = {
		"running",
		"armed",
		"armed_throttled",
		"preempted",
		"preempted_throttled",
		"throttled",
	},
	.event_names = {
		"dl_defer_arm",
		"dl_replenish",
		"dl_throttle",
		"sched_switch_in",
		"sched_switch_out",
	},
	.env_names = {
		"clk",
		"is_constr_dl",
		"yielded",
	},
	.function = {
		{
			armed_throttle,
			running_throttle,
			throttled_throttle,
			running_throttle,
			preempted_throttle,
		},
		{
			armed_throttle,
			armed_throttle,
			armed_throttled_throttle,
			running_throttle,
			preempted_throttle,
		},
		{
			armed_throttled_throttle,
			armed_throttle,
			armed_throttled_throttle,
			INVALID_STATE,
			preempted_throttled_throttle,
		},
		{
			armed_throttle,
			preempted_throttle,
			preempted_throttled_throttle,
			running_throttle,
			preempted_throttle,
		},
		{
			armed_throttled_throttle,
			preempted_throttle,
			INVALID_STATE,
			INVALID_STATE,
			preempted_throttled_throttle,
		},
		{
			armed_throttled_throttle,
			running_throttle,
			INVALID_STATE,
			INVALID_STATE,
			preempted_throttled_throttle,
		},
	},
	.initial_state = running_throttle,
	.final_states = { 1, 0, 0, 0, 0, 0 },
};
