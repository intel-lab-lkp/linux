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
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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

/* Session handle and init flag used for lazy CPU buffer init in pipe mode */
static struct perf_session *trace_dat_session;
static bool cpu_buffers_initialized;

/* Store raw tracepoint event data in per-cpu buffer for trace.dat flyrecord */
static int process_sample_event(const struct perf_tool *tool,
				union perf_event *event __maybe_unused,
				struct perf_sample *sample,
				struct machine *machine __maybe_unused)
{
	struct trace_convert *tc = container_of(tool, struct trace_convert, tool);
	struct evsel *evsel = sample->evsel;

	/* Only process tracepoint events */
	if (!trace_dat_fp || sample->raw_size == 0 ||
		!evsel ||
		evsel->core.attr.type != PERF_TYPE_TRACEPOINT)
		return 0;

	/*
	 * In pipe mode, CPU count and page size arrive via feature/tracing_data
	 * records before the first sample; initialize buffers lazily on first sample.
	 */
	if (!cpu_buffers_initialized) {
		int nr_cpus = trace_dat_session->header.env.nr_cpus_online;

		if (trace_dat_session->tevent.pevent)
			trace_dat_page_size = tep_get_page_size(trace_dat_session->tevent.pevent);

		/*
		 * nr_cpus and page size MUST come from the recording
		 * machine's own stream data (feature/tracing_data records
		 * never from local sysconf() - the converting host may be
		 * a different arch/cpu-count/page-size than the machine
		 * that recorded the data. Pipe-mode ordering guarantees
		 * (record__synthesize()) that attr -> feature ->
		 * tracing_data are always written before the first sample,
		 * so by the time we're in this callback, both values
		 * should already be populated by process_feature()/
		 * process_tracing_data().
		 */
		if (nr_cpus <= 0 || trace_dat_page_size == 0) {
			pr_err("Malformed pipe stream: missing feature/tracing_data before first sample\n");
			return -EINVAL;
		}

		if (trace_dat__init_cpu_buffers(nr_cpus) < 0) {
			pr_err("Failed to initialize CPU buffers\n");
			return -ENOMEM;
		}
		cpu_buffers_initialized = true;
	}

	if (trace_dat__collect_cpu_event(sample->cpu, sample->time,
				sample->raw_data, sample->raw_size) < 0) {
		pr_err("Failed to collect CPU event\n");
		return -ENOMEM;
	}
	tc->events_count++;

	return 0;
}

/* Process event attributes for pipe mode */
static int process_attr(const struct perf_tool *tool __maybe_unused,
		       union perf_event *event,
		       struct evlist **pevlist)
{
	return perf_event__process_attr(tool, event, pevlist);
}

/* Process feature records for pipe mode */
static int process_feature(const struct perf_tool *tool __maybe_unused,
			  struct perf_session *session,
			  union perf_event *event)
{
	return perf_event__process_feature(tool, session, event);
}

/* Process tracing data for pipe mode */
static int process_tracing_data(const struct perf_tool *tool __maybe_unused,
			       struct perf_session *session,
			       union perf_event *event)
{
	return perf_event__process_tracing_data(tool, session, event);
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

	cpu_buffers_initialized = false;

	/* Initialize tool with all required callbacks */
	perf_tool__init(&tc.tool, /*ordered_events=*/true);
	tc.tool.sample = process_sample_event;
	tc.tool.attr = process_attr;
	tc.tool.feature = process_feature;
	tc.tool.tracing_data = process_tracing_data;

	if (!opts->force) {
		int fd = open(to_trace, O_WRONLY | O_CREAT | O_EXCL, 0644);

		if (fd < 0) {
			if (errno == EEXIST)
				pr_err("Output file '%s' already exists. Use --force to overwrite.\n",
				       to_trace);
			else
				pr_err("Failed to open output file '%s': %s\n",
				       to_trace, strerror(errno));
			return -errno;
		}
		trace_dat_fp = fdopen(fd, "wb");
		if (!trace_dat_fp) {
			int err = errno;

			close(fd);
			pr_err("Failed to open output file '%s': %s\n",
			       to_trace, strerror(err));
			ret = -err;
			goto out_close;
		}
	} else {
		trace_dat_fp = fopen(to_trace, "wb");
		if (!trace_dat_fp) {
			pr_err("Failed to open output file: %s\n", to_trace);
			return -EINVAL;
		}
	}

	/* Open perf.data session - this writes trace.dat metadata sections */
	session = perf_session__new(&data, &tc.tool);
	if (IS_ERR(session)) {
		pr_err("Failed to open perf.data file\n");
		ret = PTR_ERR(session);
		goto out_close;
	}

	/* Stash session for lazy CPU buffer init on first sample (pipe and normal mode) */
	trace_dat_session = session;

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
	if (trace_dat__write_options_section1(session->tevent.pevent) < 0 ||
			trace_dat_write_failed) {
		pr_err("Failed to write options section1\n");
		ret = -EIO;
		goto out_delete;
	}
	if (trace_dat__write_options_section2(session->tevent.pevent) < 0 ||
			trace_dat_write_failed) {
		pr_err("Failed to write options section2\n");
		ret = -EIO;
		goto out_delete;
	}
	if (trace_dat__write_flyrecord_section(session->tevent.pevent) < 0 ||
			trace_dat_write_failed) {
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
	trace_dat_session = NULL;
out_close:
	if (trace_dat_fp) {
		fclose(trace_dat_fp);
		trace_dat_fp = NULL;
	}
	if (ret != 0)
		unlink(to_trace);
	return ret;
}
