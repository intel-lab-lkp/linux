/* SPDX-License-Identifier: GPL-2.0 */
#include <stdbool.h>

enum action_type {
	ACTION_NONE = 0,
	ACTION_TRACE_OUTPUT,
	ACTION_SIGNAL,
	ACTION_EXEC,
	ACTION_FIELD_N
};

struct action_options {
	enum action_type actions[ACTION_FIELD_N];
	int actions_length;
	bool action_present[ACTION_FIELD_N];

	/* For ACTION_TRACE_OUTPUT */
	char *trace_output;

	/* For ACTION_SIGNAL */
	int signal;
	int pid;

	/* For ACTION_COMMAND */
	char *command;
};

void action_options_init(struct action_options *opts);
void action_options_destroy(struct action_options *opts);
int action_add_trace_output(struct action_options *opts, char *trace_output);
int action_add_signal(struct action_options *opts, int signal, int pid);
int action_add_exec(struct action_options *opts, char *command);
int action_parse(struct action_options *opts, char *trigger);
