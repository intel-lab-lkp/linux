// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include <string.h>

#include "actions.h"

/*
 * action_options_init - initialize struct action_options
 */
void
action_options_init(struct action_options *opts)
{
	opts->actions_length = 0;
	for (int i = 0; i < ACTION_FIELD_N; i++) {
		opts->actions[i] = ACTION_NONE;
		opts->action_present[i] = false;
	}
	opts->trace_output = NULL;
}

/*
 * action_options_destroy - destroy struct action_options
 */
void
action_options_destroy(struct action_options *opts)
{
	if (opts->command)
		free(opts->command);
}

/*
 * action_add_trace_output - add an action to output trace
 */
int
action_add_trace_output(struct action_options *opts, char *trace_output)
{
	if (opts->action_present[ACTION_TRACE_OUTPUT])
		return 1;
	opts->action_present[ACTION_TRACE_OUTPUT] = true;

	opts->actions[opts->actions_length++] = ACTION_TRACE_OUTPUT;
	opts->trace_output = trace_output;

	return 0;
}

/*
 * action_add_trace_output - add an action to send signal to a process
 */
int
action_add_signal(struct action_options *opts, int signal, int pid)
{
	if (opts->action_present[ACTION_SIGNAL])
		return 1;
	opts->action_present[ACTION_SIGNAL] = true;

	opts->actions[opts->actions_length++] = ACTION_SIGNAL;
	opts->signal = signal;
	opts->pid = pid;

	return 0;
}

/*
 * action_add_exec - add an action to execute a shell command
 */
int
action_add_exec(struct action_options *opts, char *command)
{
	if (opts->action_present[ACTION_EXEC])
		return 1;
	opts->action_present[ACTION_EXEC] = true;

	opts->actions[opts->actions_length++] = ACTION_EXEC;
	if (opts->command)
		free(opts->command);
	opts->command = calloc(sizeof(char), strlen(command) + 1);
	if (!opts->command)
		return -1;
	strcpy(opts->command, command);

	return 0;
}

/*
 * action_parse - add an action based on text specification
 */
int
action_parse(struct action_options *opts, char *trigger)
{
	enum action_type type = ACTION_NONE;
	char *token;
	char trigger_c[strlen(trigger)];

	/* For ACTION_SIGNAL */
	int signal = 0, pid = 0;

	if (opts->actions_length == ACTION_FIELD_N)
		return -1;

	strcpy(trigger_c, trigger);
	token = strtok(trigger_c, ",");

	if (strcmp(token, "trace") == 0)
		type = ACTION_TRACE_OUTPUT;
	else if (strcmp(token, "signal") == 0)
		type = ACTION_SIGNAL;
	else if (strcmp(token, "exec") == 0)
		type = ACTION_EXEC;
	else
		/* Invalid trigger type */
		return -1;

	token = strtok(NULL, ",");

	switch (type) {
	case ACTION_TRACE_OUTPUT:
		/* Takes no argument */
		if (token != NULL)
			return -1;
		return action_add_trace_output(opts, "timerlat_trace.txt");
	case ACTION_SIGNAL:
		/* Takes two arguments, num (signal) and pid */
		while (token != NULL) {
			if (strlen(token) > 4 && strncmp(token, "num=", 4) == 0) {
				signal = atoi(token + 4);
			} else if (strlen(token) > 4 && strncmp(token, "pid=", 4) == 0) {
				if (strncmp(token + 4, "parent", 7) == 0)
					pid = -1;
				else
					pid = atoi(token + 4);
			} else {
				/* Invalid argument */
				return -1;
			}

			token = strtok(NULL, ",");
		}

		if (!signal || !pid)
			/* Missing argument */
			return -1;

		return action_add_signal(opts, signal, pid);
	case ACTION_EXEC:
		if (token == NULL)
			return -1;
		if (strlen(token) > 8 && strncmp(token, "command=", 8) == 0) {
			return action_add_exec(opts, token + 8);
		}
		return -1;
	default:
		return -1;
	}
}
