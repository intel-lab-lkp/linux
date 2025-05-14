/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of sssw automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

enum states_sssw {
	runnanble_sssw = 0,
	sleepable_sssw,
	sleeping_sssw,
	state_max_sssw
};

#define INVALID_STATE state_max_sssw

enum events_sssw {
	sched_set_state_runnable_sssw = 0,
	sched_set_state_sleepable_sssw,
	sched_switch_blocking_sssw,
	sched_switch_in_sssw,
	sched_switch_preempt_sssw,
	sched_switch_suspend_sssw,
	sched_switch_vain_sssw,
	sched_switch_vain_preempt_sssw,
	sched_switch_yield_sssw,
	sched_wakeup_sssw,
	event_max_sssw
};

struct automaton_sssw {
	char *state_names[state_max_sssw];
	char *event_names[event_max_sssw];
	unsigned char function[state_max_sssw][event_max_sssw];
	unsigned char initial_state;
	bool final_states[state_max_sssw];
};

static const struct automaton_sssw automaton_sssw = {
	.state_names = {
		"runnanble",
		"sleepable",
		"sleeping"
	},
	.event_names = {
		"sched_set_state_runnable",
		"sched_set_state_sleepable",
		"sched_switch_blocking",
		"sched_switch_in",
		"sched_switch_preempt",
		"sched_switch_suspend",
		"sched_switch_vain",
		"sched_switch_vain_preempt",
		"sched_switch_yield",
		"sched_wakeup"
	},
	.function = {
		{     runnanble_sssw,     sleepable_sssw,      sleeping_sssw,     runnanble_sssw,     runnanble_sssw,      INVALID_STATE,     runnanble_sssw,     runnanble_sssw,     runnanble_sssw,     runnanble_sssw },
		{     runnanble_sssw,     sleepable_sssw,      sleeping_sssw,     sleepable_sssw,     sleepable_sssw,      sleeping_sssw,      INVALID_STATE,     sleepable_sssw,      INVALID_STATE,     runnanble_sssw },
		{      INVALID_STATE,      INVALID_STATE,      INVALID_STATE,      INVALID_STATE,      INVALID_STATE,      INVALID_STATE,      INVALID_STATE,      INVALID_STATE,      INVALID_STATE,     runnanble_sssw },
	},
	.initial_state = runnanble_sssw,
	.final_states = { 1, 0, 0 },
};
