// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Red Hat Inc, Daniel Bristot de Oliveira <bristot@kernel.org>
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sched.h>

#include <linux/compiler.h>

#include "osnoise.h"

#define DEFAULT_SAMPLE_PERIOD	1000000			/* 1s */
#define DEFAULT_SAMPLE_RUNTIME	1000000			/* 1s */

/*
 * osnoise_get_cpus - return the original "osnoise/cpus" content
 *
 * It also saves the value to be restored.
 */
char *osnoise_get_cpus(struct osnoise_context *context)
{
	if (context->curr_cpus)
		return context->curr_cpus;

	if (context->orig_cpus)
		return context->orig_cpus;

	context->orig_cpus = tracefs_instance_file_read(NULL, "osnoise/cpus", NULL);

	/*
	 * The error value (NULL) is the same for tracefs_instance_file_read()
	 * and this functions, so:
	 */
	return context->orig_cpus;
}

/*
 * osnoise_set_cpus - configure osnoise to run on *cpus
 *
 * "osnoise/cpus" file is used to set the cpus in which osnoise/timerlat
 * will run. This function opens this file, saves the current value,
 * and set the cpus passed as argument.
 */
int osnoise_set_cpus(struct osnoise_context *context, char *cpus)
{
	char *orig_cpus = osnoise_get_cpus(context);
	char buffer[1024];
	int retval;

	if (!orig_cpus)
		return -1;

	context->curr_cpus = strdup(cpus);
	if (!context->curr_cpus)
		return -1;

	snprintf(buffer, ARRAY_SIZE(buffer), "%s\n", cpus);

	debug_msg("setting cpus to %s from %s", cpus, context->orig_cpus);

	retval = tracefs_instance_file_write(NULL, "osnoise/cpus", buffer);
	if (retval < 0) {
		free(context->curr_cpus);
		context->curr_cpus = NULL;
		return -1;
	}

	return 0;
}

/*
 * osnoise_restore_cpus - restore the original "osnoise/cpus"
 *
 * osnoise_set_cpus() saves the original data for the "osnoise/cpus"
 * file. This function restore the original config it was previously
 * modified.
 */
void osnoise_restore_cpus(struct osnoise_context *context)
{
	int retval;

	if (!context->orig_cpus)
		return;

	if (!context->curr_cpus)
		return;

	/* nothing to do? */
	if (!strcmp(context->orig_cpus, context->curr_cpus))
		goto out_done;

	debug_msg("restoring cpus to %s", context->orig_cpus);

	retval = tracefs_instance_file_write(NULL, "osnoise/cpus", context->orig_cpus);
	if (retval < 0)
		err_msg("could not restore original osnoise cpus\n");

out_done:
	free(context->curr_cpus);
	context->curr_cpus = NULL;
}

/*
 * osnoise_put_cpus - restore cpus config and cleanup data
 */
void osnoise_put_cpus(struct osnoise_context *context)
{
	osnoise_restore_cpus(context);

	if (!context->orig_cpus)
		return;

	free(context->orig_cpus);
	context->orig_cpus = NULL;
}

/*
 * osnoise_read_ll_config - read a long long value from a config
 *
 * returns -1 on error.
 */
static long long osnoise_read_ll_config(char *rel_path)
{
	long long retval;
	char *buffer;

	buffer = tracefs_instance_file_read(NULL, rel_path, NULL);
	if (!buffer)
		return -1;

	/* get_llong_from_str returns -1 on error */
	retval = get_llong_from_str(buffer);

	debug_msg("reading %s returned %lld\n", rel_path, retval);

	free(buffer);

	return retval;
}

/*
 * osnoise_write_ll_config - write a long long value to a config in rel_path
 *
 * returns -1 on error.
 */
static long long osnoise_write_ll_config(char *rel_path, long long value)
{
	char buffer[BUFF_U64_STR_SIZE];
	long long retval;

	snprintf(buffer, sizeof(buffer), "%lld\n", value);

	debug_msg("setting %s to %lld\n", rel_path, value);

	retval = tracefs_instance_file_write(NULL, rel_path, buffer);
	return retval;
}

/*
 * osnoise_get_runtime - return the original "osnoise/runtime_us" value
 *
 * It also saves the value to be restored.
 */
unsigned long long osnoise_get_runtime(struct osnoise_context *context)
{
	long long runtime_us;

	if (context->runtime_us != OSNOISE_TIME_INIT_VAL)
		return context->runtime_us;

	if (context->orig_runtime_us != OSNOISE_TIME_INIT_VAL)
		return context->orig_runtime_us;

	runtime_us = osnoise_read_ll_config("osnoise/runtime_us");
	if (runtime_us < 0)
		goto out_err;

	context->orig_runtime_us = runtime_us;
	return runtime_us;

out_err:
	return OSNOISE_TIME_INIT_VAL;
}

/*
 * osnoise_get_period - return the original "osnoise/period_us" value
 *
 * It also saves the value to be restored.
 */
unsigned long long osnoise_get_period(struct osnoise_context *context)
{
	long long period_us;

	if (context->period_us != OSNOISE_TIME_INIT_VAL)
		return context->period_us;

	if (context->orig_period_us != OSNOISE_TIME_INIT_VAL)
		return context->orig_period_us;

	period_us = osnoise_read_ll_config("osnoise/period_us");
	if (period_us < 0)
		goto out_err;

	context->orig_period_us = period_us;
	return period_us;

out_err:
	return OSNOISE_TIME_INIT_VAL;
}

static int __osnoise_write_runtime(struct osnoise_context *context,
				   unsigned long long runtime)
{
	int retval;

	if (context->orig_runtime_us == OSNOISE_TIME_INIT_VAL)
		return -1;

	retval = osnoise_write_ll_config("osnoise/runtime_us", runtime);
	if (retval < 0)
		return -1;

	context->runtime_us = runtime;
	return 0;
}

static int __osnoise_write_period(struct osnoise_context *context,
				  unsigned long long period)
{
	int retval;

	if (context->orig_period_us == OSNOISE_TIME_INIT_VAL)
		return -1;

	retval = osnoise_write_ll_config("osnoise/period_us", period);
	if (retval < 0)
		return -1;

	context->period_us = period;
	return 0;
}

/*
 * osnoise_set_runtime_period - set osnoise runtime and period
 *
 * Osnoise's runtime and period are related as runtime <= period.
 * Thus, this function saves the original values, and then tries
 * to set the runtime and period if they are != 0.
 */
int osnoise_set_runtime_period(struct osnoise_context *context,
			       unsigned long long runtime,
			       unsigned long long period)
{
	unsigned long long curr_runtime_us;
	unsigned long long curr_period_us;
	int retval;

	if (!period && !runtime)
		return 0;

	curr_runtime_us = osnoise_get_runtime(context);
	curr_period_us = osnoise_get_period(context);

	/* error getting any value? */
	if (curr_period_us == OSNOISE_TIME_INIT_VAL || curr_runtime_us == OSNOISE_TIME_INIT_VAL)
		return -1;

	if (!period) {
		if (runtime > curr_period_us)
			return -1;
		return __osnoise_write_runtime(context, runtime);
	} else if (!runtime) {
		if (period < curr_runtime_us)
			return -1;
		return __osnoise_write_period(context, period);
	}

	if (runtime > curr_period_us) {
		retval = __osnoise_write_period(context, period);
		if (retval)
			return -1;
		retval = __osnoise_write_runtime(context, runtime);
		if (retval)
			return -1;
	} else {
		retval = __osnoise_write_runtime(context, runtime);
		if (retval)
			return -1;
		retval = __osnoise_write_period(context, period);
		if (retval)
			return -1;
	}

	return 0;
}

/*
 * osnoise_restore_runtime_period - restore the original runtime and period
 */
void osnoise_restore_runtime_period(struct osnoise_context *context)
{
	unsigned long long orig_runtime = context->orig_runtime_us;
	unsigned long long orig_period = context->orig_period_us;
	unsigned long long curr_runtime = context->runtime_us;
	unsigned long long curr_period = context->period_us;
	int retval;

	if ((orig_runtime == OSNOISE_TIME_INIT_VAL) && (orig_period == OSNOISE_TIME_INIT_VAL))
		return;

	if ((orig_period == curr_period) && (orig_runtime == curr_runtime))
		goto out_done;

	retval = osnoise_set_runtime_period(context, orig_runtime, orig_period);
	if (retval)
		err_msg("Could not restore original osnoise runtime/period\n");

out_done:
	context->runtime_us = OSNOISE_TIME_INIT_VAL;
	context->period_us = OSNOISE_TIME_INIT_VAL;
}

/*
 * osnoise_put_runtime_period - restore original values and cleanup data
 */
void osnoise_put_runtime_period(struct osnoise_context *context)
{
	osnoise_restore_runtime_period(context);

	if (context->orig_runtime_us != OSNOISE_TIME_INIT_VAL)
		context->orig_runtime_us = OSNOISE_TIME_INIT_VAL;

	if (context->orig_period_us != OSNOISE_TIME_INIT_VAL)
		context->orig_period_us = OSNOISE_TIME_INIT_VAL;
}

/*
 * Long long option get/set/restore/put functions, generated from OSNOISE_LL_OPTIONS.
 */
#define OSNOISE_LL_OPTION(name, path, init_val)						\
static long long									\
osnoise_get_##name(struct osnoise_context *context)					\
{											\
	long long name;									\
											\
	if (context->name != (init_val))						\
		return context->name;							\
											\
	if (context->orig_##name != (init_val))						\
		return context->orig_##name;						\
											\
	name = osnoise_read_ll_config(path);						\
	if (name < 0)									\
		return (init_val);							\
											\
	context->orig_##name = name;							\
	return name;									\
}											\
											\
int osnoise_set_##name(struct osnoise_context *context, long long name)			\
{											\
	long long curr = osnoise_get_##name(context);					\
	int retval;									\
											\
	if (curr == (init_val))								\
		return -1;								\
											\
	retval = osnoise_write_ll_config(path, name);					\
	if (retval < 0)									\
		return -2;								\
											\
	context->name = name;								\
	return 0;									\
}											\
											\
void osnoise_restore_##name(struct osnoise_context *context)				\
{											\
	int retval;									\
											\
	if (context->orig_##name == (init_val))						\
		return;									\
											\
	if (context->orig_##name == context->name)					\
		goto out_done_##name;							\
											\
	retval = osnoise_write_ll_config(path, context->orig_##name);			\
	if (retval < 0)									\
		err_msg("Could not restore original " #name "\n");			\
											\
out_done_##name:									\
	context->name = (init_val);							\
}											\
											\
static void osnoise_put_##name(struct osnoise_context *context)				\
{											\
	osnoise_restore_##name(context);						\
											\
	if (context->orig_##name == (init_val))						\
		return;									\
											\
	context->orig_##name = (init_val);						\
}
OSNOISE_LL_OPTIONS
#undef OSNOISE_LL_OPTION

static int osnoise_options_get_option(char *option)
{
	char *options = tracefs_instance_file_read(NULL, "osnoise/options", NULL);
	char no_option[128];
	int retval = 0;
	char *opt;

	if (!options)
		return OSNOISE_OPTION_INIT_VAL;

	/*
	 * Check first if the option is disabled.
	 */
	snprintf(no_option, sizeof(no_option), "NO_%s", option);

	opt = strstr(options, no_option);
	if (opt)
		goto out_free;

	/*
	 * Now that it is not disabled, if the string is there, it is
	 * enabled. If the string is not there, the option does not exist.
	 */
	opt = strstr(options, option);
	if (opt)
		retval = 1;
	else
		retval = OSNOISE_OPTION_INIT_VAL;

out_free:
	free(options);
	return retval;
}

static int osnoise_options_set_option(char *option, bool onoff)
{
	char no_option[128];

	if (onoff)
		return tracefs_instance_file_write(NULL, "osnoise/options", option);

	snprintf(no_option, sizeof(no_option), "NO_%s", option);

	return tracefs_instance_file_write(NULL, "osnoise/options", no_option);
}

/*
 * Flag option get/set/restore/put functions, generated from OSNOISE_FLAG_OPTIONS.
 */
#define OSNOISE_FLAG_OPTION(name, option_str)						\
static int osnoise_get_##name(struct osnoise_context *context)				\
{											\
	if (context->opt_##name != OSNOISE_OPTION_INIT_VAL)				\
		return context->opt_##name;						\
											\
	if (context->orig_opt_##name != OSNOISE_OPTION_INIT_VAL)			\
		return context->orig_opt_##name;					\
											\
	context->orig_opt_##name = osnoise_options_get_option(option_str);		\
	return context->orig_opt_##name;						\
}											\
											\
int osnoise_set_##name(struct osnoise_context *context, bool onoff)			\
{											\
	int val = osnoise_get_##name(context);						\
	int retval;									\
											\
	if (val == OSNOISE_OPTION_INIT_VAL)						\
		return -1;								\
											\
	if (val == onoff)								\
		return 0;								\
											\
	retval = osnoise_options_set_option(option_str, onoff);				\
	if (retval < 0)									\
		return -2;								\
											\
	context->opt_##name = onoff;							\
	return 0;									\
}											\
											\
void osnoise_restore_##name(struct osnoise_context *context)				\
{											\
	int retval;									\
											\
	if (context->orig_opt_##name == OSNOISE_OPTION_INIT_VAL)			\
		return;									\
											\
	if (context->orig_opt_##name == context->opt_##name)				\
		goto out_done_##name;							\
											\
	retval = osnoise_options_set_option(option_str, context->orig_opt_##name);	\
	if (retval < 0)									\
		err_msg("Could not restore original " option_str " option\n");		\
											\
out_done_##name:									\
	context->opt_##name = OSNOISE_OPTION_INIT_VAL;					\
}											\
											\
static void osnoise_put_##name(struct osnoise_context *context)				\
{											\
	osnoise_restore_##name(context);						\
											\
	if (context->orig_opt_##name == OSNOISE_OPTION_INIT_VAL)			\
		return;									\
											\
	context->orig_opt_##name = OSNOISE_OPTION_INIT_VAL;				\
}
OSNOISE_FLAG_OPTIONS
#undef OSNOISE_FLAG_OPTION

enum {
	FLAG_CONTEXT_NEWLY_CREATED	= (1 << 0),
	FLAG_CONTEXT_DELETED		= (1 << 1),
};

/*
 * osnoise_get_context - increase the usage of a context and return it
 */
int osnoise_get_context(struct osnoise_context *context)
{
	int ret;

	if (context->flags & FLAG_CONTEXT_DELETED) {
		ret = -1;
	} else {
		context->ref++;
		ret = 0;
	}

	return ret;
}

/*
 * osnoise_context_alloc - alloc an osnoise_context
 *
 * The osnoise context contains the information of the "osnoise/" configs.
 * It is used to set and restore the config.
 */
struct osnoise_context *osnoise_context_alloc(void)
{
	struct osnoise_context *context;

	context = calloc_fatal(1, sizeof(*context));

#define OSNOISE_LL_OPTION(name, path, init_val)			\
	context->orig_##name	 = (init_val);			\
	context->name		 = (init_val);
#define OSNOISE_FLAG_OPTION(name, option_str)			\
	context->orig_opt_##name = OSNOISE_OPTION_INIT_VAL; 	\
	context->opt_##name	 = OSNOISE_OPTION_INIT_VAL;
	OSNOISE_LL_OPTIONS
	OSNOISE_FLAG_OPTIONS
#undef OSNOISE_LL_OPTION
#undef OSNOISE_FLAG_OPTION

	osnoise_get_context(context);

	return context;
}

/*
 * osnoise_put_context - put the osnoise_put_context
 *
 * If there is no other user for the context, the original data
 * is restored.
 */
void osnoise_put_context(struct osnoise_context *context)
{
	if (--context->ref < 1)
		context->flags |= FLAG_CONTEXT_DELETED;

	if (!(context->flags & FLAG_CONTEXT_DELETED))
		return;

	osnoise_put_cpus(context);
	osnoise_put_runtime_period(context);

#define OSNOISE_LL_OPTION(name, path, init_val)	osnoise_put_##name(context);
#define OSNOISE_FLAG_OPTION(name, option_str)	osnoise_put_##name(context);
	OSNOISE_LL_OPTIONS
	OSNOISE_FLAG_OPTIONS
#undef OSNOISE_LL_OPTION
#undef OSNOISE_FLAG_OPTION

	free(context);
}

/*
 * osnoise_destroy_tool - disable trace, restore configs and free data
 */
void osnoise_destroy_tool(struct osnoise_tool *top)
{
	if (!top)
		return;

	trace_instance_destroy(&top->trace);

	if (top->context)
		osnoise_put_context(top->context);

	free(top);
}

/*
 * osnoise_init_tool - init an osnoise tool
 *
 * It allocs data, create a context to store data and
 * creates a new trace instance for the tool.
 */
struct osnoise_tool *osnoise_init_tool(char *tool_name)
{
	struct osnoise_tool *top;

	top = calloc_fatal(1, sizeof(*top));
	top->context = osnoise_context_alloc();

	if (trace_instance_init(&top->trace, tool_name)) {
		osnoise_destroy_tool(top);
		return NULL;
	}

	return top;
}

/*
 * osnoise_init_trace_tool - init a tracer instance to trace osnoise events
 */
struct osnoise_tool *osnoise_init_trace_tool(const char *tracer)
{
	struct osnoise_tool *trace;
	int retval;

	trace = osnoise_init_tool("osnoise_trace");
	if (!trace)
		return NULL;

	retval = tracefs_event_enable(trace->trace.inst, "osnoise", NULL);
	if (retval < 0 && !errno) {
		err_msg("Could not find osnoise events\n");
		goto out_err;
	}

	retval = enable_tracer_by_name(trace->trace.inst, tracer);
	if (retval) {
		err_msg("Could not enable %s tracer for tracing\n", tracer);
		goto out_err;
	}

	return trace;
out_err:
	osnoise_destroy_tool(trace);
	return NULL;
}

bool osnoise_trace_is_off(struct osnoise_tool *tool, struct osnoise_tool *record)
{
	/*
	 * The tool instance is always present, it is the one used to collect
	 * data.
	 */
	if (!tracefs_trace_is_on(tool->trace.inst))
		return true;

	/*
	 * The trace record instance is only enabled when -t is set. IOW, when the system
	 * is tracing.
	 */
	return record && !tracefs_trace_is_on(record->trace.inst);
}

/*
 * osnoise_report_missed_events - report number of events dropped by trace
 * buffer
 */
void
osnoise_report_missed_events(struct osnoise_tool *tool)
{
	unsigned long long total_events;

	if (tool->trace.missed_events == UINT64_MAX)
		printf("unknown number of events missed, results might not be accurate\n");
	else if (tool->trace.missed_events > 0) {
		total_events = tool->trace.processed_events + tool->trace.missed_events;

		printf("%lld (%.2f%%) events missed, results might not be accurate\n",
		       tool->trace.missed_events,
		       (double) tool->trace.missed_events / total_events * 100.0);
	}
}

/*
 * osnoise_apply_config - apply osnoise configs to the initialized tool
 */
int
osnoise_apply_config(struct osnoise_tool *tool, struct osnoise_params *params)
{
	int retval;

	params->common.kernel_workload = true;

	if (params->runtime || params->period) {
		retval = osnoise_set_runtime_period(tool->context,
						    params->runtime,
						    params->period);
	} else {
		retval = osnoise_set_runtime_period(tool->context,
						    DEFAULT_SAMPLE_PERIOD,
						    DEFAULT_SAMPLE_RUNTIME);
	}

	if (retval) {
		err_msg("Failed to set runtime and/or period\n");
		goto out_err;
	}

	retval = osnoise_set_tracing_thresh(tool->context, params->threshold);
	if (retval) {
		err_msg("Failed to set tracing_thresh\n");
		goto out_err;
	}

	return common_apply_config(tool, &params->common);

out_err:
	return -1;
}

int osnoise_enable(struct osnoise_tool *tool)
{
	struct osnoise_params *params = to_osnoise_params(tool->params);
	int retval;

	/*
	 * Start the tracer here, after having set all instances.
	 *
	 * Let the trace instance start first for the case of hitting a stop
	 * tracing while enabling other instances. The trace instance is the
	 * one with most valuable information.
	 */
	if (tool->record)
		trace_instance_start(&tool->record->trace);
	trace_instance_start(&tool->trace);

	if (params->common.warmup > 0) {
		debug_msg("Warming up for %d seconds\n", params->common.warmup);
		sleep(params->common.warmup);
		if (stop_tracing)
			return -1;

		/*
		 * Clean up the buffer. The osnoise workload do not run
		 * with tracing off to avoid creating a performance penalty
		 * when not needed.
		 */
		retval = tracefs_instance_file_write(tool->trace.inst, "trace", "");
		if (retval < 0) {
			debug_msg("Error cleaning up the buffer");
			return retval;
		}
	}

	retval = osn_set_stop(tool);
	if (retval)
		return retval;

	return 0;
}

__noreturn static void osnoise_usage(int err)
{
	int i;

	static const char *msg[] = {
		"",
		"osnoise version " VERSION,
		"",
		"  usage: [rtla] osnoise [MODE] ...",
		"",
		"  modes:",
		"     top   - prints the summary from osnoise tracer",
		"     hist  - prints a histogram of osnoise samples",
		"",
		"if no MODE is given, the top mode is called, passing the arguments",
		NULL,
	};

	for (i = 0; msg[i]; i++)
		fprintf(stderr, "%s\n", msg[i]);
	exit(err);
}

int osnoise_main(int argc, char *argv[])
{
	if (argc == 0)
		goto usage;

	/*
	 * if osnoise was called without any argument, run the
	 * default cmdline.
	 */
	if (argc == 1) {
		run_tool(&osnoise_top_ops, argc, argv);
		exit(0);
	}

	if ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0)) {
		osnoise_usage(129);
	} else if (str_has_prefix(argv[1], "-")) {
		/* the user skipped the tool, call the default one */
		run_tool(&osnoise_top_ops, argc, argv);
		exit(0);
	} else if (strcmp(argv[1], "top") == 0) {
		run_tool(&osnoise_top_ops, argc-1, &argv[1]);
		exit(0);
	} else if (strcmp(argv[1], "hist") == 0) {
		run_tool(&osnoise_hist_ops, argc-1, &argv[1]);
		exit(0);
	}

usage:
	osnoise_usage(129);
}

int hwnoise_main(int argc, char *argv[])
{
	run_tool(&osnoise_top_ops, argc, argv);
	exit(0);
}
