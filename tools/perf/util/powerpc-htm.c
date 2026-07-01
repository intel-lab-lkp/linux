// SPDX-License-Identifier: GPL-2.0
/*
 * HTM support
 */

#include "../../../util/record.h"
#include "evlist.h"
#include "evsel.h"
#include "session.h"
#include "debug.h"
#include <internal/xyarray.h>
#include <linux/string.h>
#include "color.h"
#include <inttypes.h>
#include "powerpc-htm.h"
#include <errno.h>

struct perf_session;

struct powerpc_htm {
	struct auxtrace			auxtrace;
	struct auxtrace_queues		queues;
	struct auxtrace_heap		heap;
	u32				auxtrace_type;
	struct perf_session		*session;
	struct machine			*machine;
	u32				pmu_type;
	char				htmbin_file[64];
};

/*
 * Check if HTM events have more data to collect.
 *
 * This function reads the HTM event counts. When the kernel driver
 * has more data available, it returns a non-zero count. When all
 * data has been collected, it returns zero.
 *
 * Returns: 1 if more data exists, 0 if collection is complete
 */
int arch_perf_record__need_read(struct evlist *evlist)
{
	struct evsel *evsel;
	int found_htm = 0;

	/* there was an error during record__open */
	if (!evlist)
		return 0;

	/* First, check if any HTM events exist */
	evlist__for_each_entry(evlist, evsel) {
		if (strstr(evsel->name, "htm") != NULL)
			found_htm = 1;
	}

	if (!found_htm)
		return 0;

	/* Read HTM event counts to check if more data is available */
	evlist__for_each_entry(evlist, evsel) {
		struct xyarray *xy = evsel->core.sample_id;

		if (strstr(evsel->name, "htm") == NULL)
			continue;

		if (xy == NULL || evsel->core.fd == NULL)
			continue;
		if (xyarray__max_x(evsel->core.fd) != xyarray__max_x(xy) ||
			xyarray__max_y(evsel->core.fd) != xyarray__max_y(xy)) {
			pr_debug("Unmatched FD vs. sample ID: skip reading LOST count\n");
			continue;
		}

		for (int x = 0; x < xyarray__max_x(xy); x++) {
			for (int y = 0; y < xyarray__max_y(xy); y++) {
				struct perf_counts_values count;

				if (!strcmp(evsel->name, "dummy:u"))
					continue;

				if (strstr(evsel->name, "htm")) {
					perf_evsel__read(&evsel->core, x, y, &count);
					y = xyarray__max_y(xy);
					x = xyarray__max_x(xy);
				}
				if (!count.val)
					return 0;
			}
		}
	}

	return 1;
}

static void powerpc_htm_dump_event(size_t len)
{
	const char *color = PERF_COLOR_BLUE;

	color_fprintf(stdout, color,
			". ... HTM PMU data: size %zu bytes\n",
			len);
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
	struct powerpc_htm *htm = container_of(session->auxtrace, struct powerpc_htm,
					     auxtrace);
	struct auxtrace_queues *queues = &htm->queues;
	unsigned int i;

	for (i = 0; i < queues->nr_queues; i++)
		zfree(&queues->queue_array[i].priv);

	auxtrace_queues__free(queues);
}

static void powerpc_htm_free(struct perf_session *session)
{
	struct powerpc_htm *htm = container_of(session->auxtrace, struct powerpc_htm,
					     auxtrace);

	powerpc_htm_free_events(session);
	session->auxtrace = NULL;
	free(htm);
}
static const char * const powerpc_htm_info_fmts[] = {
	[POWERPC_HTM_TYPE]		= "  PMU Type           %"PRId64"\n",
};

static void powerpc_htm_print_info(__u64 *arr)
{
	if (!dump_trace)
		return;

	fprintf(stdout, powerpc_htm_info_fmts[POWERPC_HTM_TYPE], arr[POWERPC_HTM_TYPE]);
}

int powerpc_htm_process_auxtrace_info(union perf_event *event,
				  struct perf_session *session)
{
	struct perf_record_auxtrace_info *auxtrace_info = &event->auxtrace_info;
	struct evsel *evsel = evlist__event2evsel(session->evlist, event);
	u32 nodeindex, nodalchipindex, coreindexonchip;
	int config = (evsel->core.attr.config);
	size_t min_sz = sizeof(u64) * POWERPC_HTM_TYPE;
	struct powerpc_htm *htm;
	int err;
	FILE *fp;

	nodeindex = (config >> 4) & 0xff;
	nodalchipindex = (config >> 12) & 0xff;
	coreindexonchip = (config >> 20) & 0xff;

	if (auxtrace_info->header.size < sizeof(struct perf_record_auxtrace_info) +
					min_sz)
		return -EINVAL;

	htm = zalloc(sizeof(struct powerpc_htm));
	if (!htm)
		return -ENOMEM;

	err = auxtrace_queues__init(&htm->queues);
	if (err)
		goto err_free;

	htm->session = session;
	htm->machine = &session->machines.host; /* No kvm support */
	htm->auxtrace_type = auxtrace_info->type;
	htm->pmu_type = auxtrace_info->priv[POWERPC_HTM_TYPE];

	htm->auxtrace.process_event = powerpc_htm_process_event;
	htm->auxtrace.process_auxtrace_event = powerpc_htm_process_auxtrace_event;
	htm->auxtrace.flush_events = powerpc_htm_flush;
	htm->auxtrace.free_events = powerpc_htm_free_events;
	htm->auxtrace.free = powerpc_htm_free;
	session->auxtrace = &htm->auxtrace;

	snprintf(htm->htmbin_file, sizeof(htm->htmbin_file), "htm.bin.n%d.p%d.c%d", nodeindex, nodalchipindex, coreindexonchip);
	fp = fopen(htm->htmbin_file, "w");
	if (!fp) {
		pr_err("Failed to create %s: %s\n", htm->htmbin_file, strerror(errno));
		return -errno;
	}
	fclose(fp);

	powerpc_htm_print_info(&auxtrace_info->priv[0]);

	err = auxtrace_queues__process_index(&htm->queues, session);
	if (err)
		goto err_free_queues;

	return 0;

err_free_queues:
	auxtrace_queues__free(&htm->queues);
	session->auxtrace = NULL;

err_free:
	free(htm);
	return err;
}
