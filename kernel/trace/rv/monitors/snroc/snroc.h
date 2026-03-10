/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of snroc automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME snroc

enum states_snroc {
	enqueued_snroc,
	dequeued_snroc,
	dequeued_running_snroc,
	own_context_snroc,
	state_max_snroc,
};

#define INVALID_STATE state_max_snroc

enum events_snroc {
	sched_dequeue_snroc,
	sched_enqueue_snroc,
	sched_set_state_snroc,
	sched_switch_in_snroc,
	sched_switch_out_snroc,
	event_max_snroc,
};

struct automaton_snroc {
	char *state_names[state_max_snroc];
	char *event_names[event_max_snroc];
	unsigned char function[state_max_snroc][event_max_snroc];
	unsigned char initial_state;
	bool final_states[state_max_snroc];
};

static const struct automaton_snroc automaton_snroc = {
	.state_names = {
		"enqueued",
		"dequeued",
		"dequeued_running",
		"own_context",
	},
	.event_names = {
		"sched_dequeue",
		"sched_enqueue",
		"sched_set_state",
		"sched_switch_in",
		"sched_switch_out",
	},
	.function = {
		{
			dequeued_snroc,
			INVALID_STATE,
			INVALID_STATE,
			own_context_snroc,
			INVALID_STATE,
		},
		{
			INVALID_STATE,
			enqueued_snroc,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
		},
		{
			INVALID_STATE,
			own_context_snroc,
			dequeued_running_snroc,
			INVALID_STATE,
			dequeued_snroc,
		},
		{
			dequeued_running_snroc,
			INVALID_STATE,
			own_context_snroc,
			INVALID_STATE,
			enqueued_snroc,
		},
	},
	.initial_state = enqueued_snroc,
	.final_states = { 1, 0, 0, 0 },
};
