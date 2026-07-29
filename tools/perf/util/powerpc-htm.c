// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <linux/zalloc.h>
#include "util/evsel.h"
#include "util/evlist.h"
#include "util/session.h"
#include "auxtrace.h"
#include "color.h"
#include "powerpc-htm.h"
#include "debug.h"
#include "sample.h"

struct powerpc_htm {
	struct auxtrace		auxtrace;
	struct auxtrace_queues	queues;
	struct auxtrace_heap	heap;
	u32			auxtrace_type;
	struct perf_session	*session;
	struct machine		*machine;
};

static void powerpc_htm_dump_event(u64 len)
{
	const char *color = PERF_COLOR_BLUE;

	if (dump_trace) {
		color_fprintf(stdout, color,
			". ... HTM PMU data: size %" PRIu64 " bytes\n", len);
	}
}

static int powerpc_htm_process_event(struct perf_session *session __maybe_unused,
				     union perf_event *event __maybe_unused,
				     struct perf_sample *sample __maybe_unused,
				     const struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static int powerpc_htm_process_auxtrace_event(struct perf_session *session __maybe_unused,
					      union perf_event *event,
					      const struct perf_tool *tool __maybe_unused)
{
	if (dump_trace)
		powerpc_htm_dump_event(event->auxtrace.size);

	return 0;
}

static int powerpc_htm_flush(struct perf_session *session __maybe_unused,
			     const struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static void powerpc_htm_free_events(struct perf_session *session)
{
	struct powerpc_htm *htm;

	if (!session || !session->auxtrace)
		return;

	htm = container_of(session->auxtrace, struct powerpc_htm, auxtrace);
	auxtrace_queues__free(&htm->queues);
}

static void powerpc_htm_free(struct perf_session *session)
{
	struct powerpc_htm *htm;

	if (!session || !session->auxtrace)
		return;

	htm = container_of(session->auxtrace, struct powerpc_htm, auxtrace);
	powerpc_htm_free_events(session);
	session->auxtrace = NULL;
	free(htm);
}

int powerpc_htm_process_auxtrace_info(union perf_event *event,
				      struct perf_session *session)
{
	struct perf_record_auxtrace_info *auxtrace_info = &event->auxtrace_info;
	struct powerpc_htm *htm;
	int err;

	if (auxtrace_info->header.size < sizeof(struct perf_record_auxtrace_info) +
					 HTM_AUXTRACE_PRIV_FIXED)
		return -EINVAL;

	htm = zalloc(sizeof(struct powerpc_htm));
	if (!htm)
		return -ENOMEM;

	err = auxtrace_queues__init(&htm->queues);
	if (err) {
		free(htm);
		return err;
	}

	htm->session = session;
	htm->machine = &session->machines.host;
	htm->auxtrace.process_event = powerpc_htm_process_event;
	htm->auxtrace.process_auxtrace_event = powerpc_htm_process_auxtrace_event;
	htm->auxtrace.flush_events = powerpc_htm_flush;
	htm->auxtrace.free_events = powerpc_htm_free_events;
	htm->auxtrace.free = powerpc_htm_free;
	session->auxtrace = &htm->auxtrace;

	return 0;
}
