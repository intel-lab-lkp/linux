/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of sts automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

enum states_sts {
	thread_sts = 0,
	disable_to_switch_sts,
	enable_to_exit_sts,
	scheduling_sts,
	switching_sts,
	state_max_sts
};

#define INVALID_STATE state_max_sts

enum events_sts {
	irq_disable_sts = 0,
	irq_enable_sts,
	sched_switch_sts,
	sched_switch_vain_sts,
	schedule_entry_sts,
	schedule_exit_sts,
	event_max_sts
};

struct automaton_sts {
	char *state_names[state_max_sts];
	char *event_names[event_max_sts];
	unsigned char function[state_max_sts][event_max_sts];
	unsigned char initial_state;
	bool final_states[state_max_sts];
};

static const struct automaton_sts automaton_sts = {
	.state_names = {
		"thread",
		"disable_to_switch",
		"enable_to_exit",
		"scheduling",
		"switching"
	},
	.event_names = {
		"irq_disable",
		"irq_enable",
		"sched_switch",
		"sched_switch_vain",
		"schedule_entry",
		"schedule_exit"
	},
	.function = {
		{            thread_sts,            thread_sts,         INVALID_STATE,         INVALID_STATE,        scheduling_sts,         INVALID_STATE },
		{         INVALID_STATE,        scheduling_sts,         switching_sts,         switching_sts,         INVALID_STATE,         INVALID_STATE },
		{    enable_to_exit_sts,    enable_to_exit_sts,         INVALID_STATE,         INVALID_STATE,         INVALID_STATE,            thread_sts },
		{ disable_to_switch_sts,         INVALID_STATE,         INVALID_STATE,         INVALID_STATE,         INVALID_STATE,         INVALID_STATE },
		{         INVALID_STATE,    enable_to_exit_sts,         INVALID_STATE,         INVALID_STATE,         INVALID_STATE,         INVALID_STATE },
	},
	.initial_state = thread_sts,
	.final_states = { 1, 0, 0, 0, 0 },
};
