// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2026, IBM Corporation
 * Author: Tanushree Shah <tshah@linux.ibm.com>
 *
 * data-convert-trace.c
 *
 * Implements perf.data to trace.dat format conversion for tracepoint events.
 */

#include <errno.h>
#include <inttypes.h>
#include <linux/compiler.h>
#include <linux/err.h>

#include "data-convert.h"
#include "session.h"
#include "evsel.h"
#include "tool.h"
#include "debug.h"
#include "trace-dat.h"
#include "trace-event.h"
#include "event.h"
#include "sample.h"
#include "evlist.h"

struct trace_convert {
	struct perf_tool tool;
	u64 events_count;
};

/* Store raw tracepoint event data in per-cpu buffer for trace.dat flyrecord */
static int process_sample_event(const struct perf_tool *tool,
				union perf_event *event __maybe_unused,
				struct perf_sample *sample,
				struct evsel *evsel,
				struct machine *machine __maybe_unused)
{
	struct trace_convert *tc = container_of(tool, struct trace_convert, tool);

	/* Collect raw tracepoint data per-cpu */
	if (trace_dat_fp && sample->raw_size > 0 &&
	    evsel->core.attr.type == PERF_TYPE_TRACEPOINT) {
		if (trace_dat__collect_cpu_event(sample->cpu, sample->time,
					sample->raw_data, sample->raw_size) < 0) {
			pr_err("Failed to collect CPU event\n");
			return -ENOMEM;
		}
		tc->events_count++;
	}

	return 0;
}

/* Convert perf.data tracepoint events to trace.dat format */
int trace_convert__perf2dat(const char *input, const char *to_trace,
			   struct perf_data_convert_opts *opts)
{
	struct perf_session *session;
	struct trace_convert tc = {
		.events_count = 0,
	};
	struct perf_data data = {
		.path = input,
		.mode = PERF_DATA_MODE_READ,
		.force = opts->force,
	};
	int ret = -EINVAL;
	bool cpu_buffers_initialized = false;

	/* Initialize tool with all required callbacks */
	perf_tool__init(&tc.tool, /*ordered_events=*/true);
	tc.tool.sample = process_sample_event;

	/* Open output trace.dat file */
	trace_dat_fp = fopen(to_trace, "wb");
	if (!trace_dat_fp) {
		pr_err("Failed to open output file: %s\n", to_trace);
		return -EINVAL;
	}

	/* Open perf.data session - this writes trace.dat metadata sections */
	session = perf_session__new(&data, &tc.tool);
	if (IS_ERR(session)) {
		pr_err("Failed to open perf.data file\n");
		ret = PTR_ERR(session);
		goto out_close;
	}

	/* Initialize per-CPU buffers for flyrecord data */
	if (session->tevent.pevent) {
		trace_dat_page_size = tep_get_page_size(session->tevent.pevent);
		if (trace_dat__init_cpu_buffers(session->header.env.nr_cpus_online) < 0) {
			pr_err("Failed to initialize CPU buffers\n");
			ret = -ENOMEM;
			goto out_delete;
		}
		cpu_buffers_initialized = true;
	}

	/* Process all events - collects raw data per-cpu */
	ret = perf_session__process_events(session);
	if (ret < 0) {
		pr_err("Failed to process events\n");
		goto out_delete;
	}

	/* Skip file creation if no tracepoint events found */
	if (tc.events_count == 0) {
		pr_warning("No tracepoint events found in '%s', skipping trace.dat creation\n",
			input);
		ret = -EINVAL;
		goto out_delete;
	}

	/* Write trace.dat options and flyrecord sections */
	if (trace_dat__write_options_section1() < 0) {
		pr_err("Failed to write options section1\n");
		ret = -EIO;
		goto out_delete;
	}
	if (trace_dat__write_options_section2() < 0) {
		pr_err("Failed to write options section2\n");
		ret = -EIO;
		goto out_delete;
	}
	if (trace_dat__write_flyrecord_section() < 0) {
		pr_err("Failed to write flyrecord section\n");
		ret = -EIO;
		goto out_delete;
	}

	pr_info("[ perf data convert: Converted '%s' into trace.dat format '%s' ]\n",
		input, to_trace);
	pr_info("[ perf data convert: Converted %llu events ]\n",
		(unsigned long long)tc.events_count);

	ret = 0;

out_delete:
	if (cpu_buffers_initialized)
		trace_dat__free_cpu_buffers();
	perf_session__delete(session);
out_close:
	if (trace_dat_fp) {
		fclose(trace_dat_fp);
		trace_dat_fp = NULL;
	}
	if (ret != 0)
		unlink(to_trace);
	return ret;
}
