/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of nrp automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

enum states_nrp {
	any_thread_running_nrp = 0,
	rescheduling_nrp,
	state_max_nrp
};

#define INVALID_STATE state_max_nrp

enum events_nrp {
	sched_need_resched_nrp = 0,
	sched_switch_other_nrp,
	sched_switch_preempt_nrp,
	sched_switch_vain_nrp,
	sched_switch_vain_preempt_nrp,
	event_max_nrp
};

struct automaton_nrp {
	char *state_names[state_max_nrp];
	char *event_names[event_max_nrp];
	unsigned char function[state_max_nrp][event_max_nrp];
	unsigned char initial_state;
	bool final_states[state_max_nrp];
};

static const struct automaton_nrp automaton_nrp = {
	.state_names = {
		"any_thread_running",
		"rescheduling"
	},
	.event_names = {
		"sched_need_resched",
		"sched_switch_other",
		"sched_switch_preempt",
		"sched_switch_vain",
		"sched_switch_vain_preempt"
	},
	.function = {
		{       rescheduling_nrp, any_thread_running_nrp,           INVALID_STATE, any_thread_running_nrp,           INVALID_STATE },
		{       rescheduling_nrp, any_thread_running_nrp, any_thread_running_nrp, any_thread_running_nrp, any_thread_running_nrp },
	},
	.initial_state = any_thread_running_nrp,
	.final_states = { 1, 0 },
};
