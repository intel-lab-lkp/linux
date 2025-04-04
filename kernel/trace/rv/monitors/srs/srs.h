/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of srs automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

enum states_srs {
	running_srs = 0,
	preempted_srs,
	preempted_sleepable_srs,
	resched_out_srs,
	resched_out_sleepable_srs,
	resched_sleepable_srs,
	rescheduling_srs,
	sleepable_srs,
	sleeping_srs,
	waking_srs,
	state_max_srs
};

#define INVALID_STATE state_max_srs

enum events_srs {
	sched_need_resched_srs = 0,
	sched_need_resched_lazy_srs,
	sched_set_state_runnable_srs,
	sched_set_state_sleepable_srs,
	sched_switch_blocking_srs,
	sched_switch_in_srs,
	sched_switch_preempt_srs,
	sched_switch_suspend_srs,
	sched_switch_vain_srs,
	sched_switch_yield_srs,
	sched_wakeup_srs,
	event_max_srs
};

struct automaton_srs {
	char *state_names[state_max_srs];
	char *event_names[event_max_srs];
	unsigned char function[state_max_srs][event_max_srs];
	unsigned char initial_state;
	bool final_states[state_max_srs];
};

static const struct automaton_srs automaton_srs = {
	.state_names = {
		"running",
		"preempted",
		"preempted_sleepable",
		"resched_out",
		"resched_out_sleepable",
		"resched_sleepable",
		"rescheduling",
		"sleepable",
		"sleeping",
		"waking"
	},
	.event_names = {
		"sched_need_resched",
		"sched_need_resched_lazy",
		"sched_set_state_runnable",
		"sched_set_state_sleepable",
		"sched_switch_blocking",
		"sched_switch_in",
		"sched_switch_preempt",
		"sched_switch_suspend",
		"sched_switch_vain",
		"sched_switch_yield",
		"sched_wakeup"
	},
	.function = {
		{          rescheduling_srs,          rescheduling_srs,               running_srs,             sleepable_srs,              sleeping_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,               running_srs,             preempted_srs,               running_srs },
		{           resched_out_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,               running_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE },
		{ resched_out_sleepable_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             sleepable_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,                waking_srs },
		{             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,          rescheduling_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE },
		{             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,     resched_sleepable_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE },
		{     resched_sleepable_srs,             INVALID_STATE,          rescheduling_srs,     resched_sleepable_srs,              sleeping_srs,             INVALID_STATE,   preempted_sleepable_srs,              sleeping_srs,             sleepable_srs,             INVALID_STATE,          rescheduling_srs },
		{          rescheduling_srs,             INVALID_STATE,          rescheduling_srs,     resched_sleepable_srs,              sleeping_srs,             INVALID_STATE,             preempted_srs,             INVALID_STATE,               running_srs,             preempted_srs,          rescheduling_srs },
		{     resched_sleepable_srs,     resched_sleepable_srs,               running_srs,             sleepable_srs,              sleeping_srs,             INVALID_STATE,   preempted_sleepable_srs,              sleeping_srs,             sleepable_srs,             INVALID_STATE,               running_srs },
		{             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,                waking_srs },
		{           resched_out_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,               running_srs,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE,             INVALID_STATE },
	},
	.initial_state = running_srs,
	.final_states = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
