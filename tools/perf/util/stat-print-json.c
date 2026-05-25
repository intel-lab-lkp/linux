// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/compiler.h>
#include <linux/kernel.h>

#include "cpumap.h"
#include "evlist.h"
#include "evsel.h"
#include "stat-print.h"
#include "stat.h"
#include "thread_map.h"

static const char *metric_threshold_classify__str(enum metric_threshold_classify thresh)
{
	const char *const strs[] = {
		"unknown", "bad", "nearly bad", "less good", "good",
	};
	_Static_assert(ARRAY_SIZE(strs) - 1 == METRIC_THRESHOLD_GOOD, "missing enum value");
	return strs[thresh];
}

/**
 * struct json_print_state - Print state context for JSON output.
 * @fp: File descriptor to output to.
 * @timestamp: Formatted interval timestamp (optional).
 */
struct json_print_state {
	FILE *fp;
	char timestamp[64];
};

/**
 * struct json_metric_only_print_state - Metric-only print state context for JSON output.
 * @fp: File descriptor to output to.
 * @timestamp: Formatted interval timestamp (optional).
 * @evlist: Evlist to query entries from.
 * @last_aggr_idx: The aggregation index of the last printed metric.
 * @first_in_group: Whether the current metric is the first in its group.
 */
struct json_metric_only_print_state {
	FILE *fp;
	char timestamp[64];
	struct evlist *evlist;
	int last_aggr_idx;
	bool first_in_group;
};

/**
 * print_aggr_id_json - Print the aggregation prefix for JSON format.
 *
 * Copied and adapted from stat-display.c.
 */
static void print_aggr_id_json(const struct perf_stat_config *config, FILE *output,
			       struct evsel *evsel, struct aggr_cpu_id id, int aggr_nr)
{
	switch (config->aggr_mode) {
	case AGGR_CORE:
		fprintf(output, "\"core\" : \"S%d-D%d-C%d\", \"counters\" : %d, ", id.socket,
			id.die, id.core, aggr_nr);
		break;
	case AGGR_CACHE:
		fprintf(output, "\"cache\" : \"S%d-D%d-L%d-ID%d\", \"counters\" : %d, ", id.socket,
			id.die, id.cache_lvl, id.cache, aggr_nr);
		break;
	case AGGR_CLUSTER:
		fprintf(output, "\"cluster\" : \"S%d-D%d-CLS%d\", \"counters\" : %d, ", id.socket,
			id.die, id.cluster, aggr_nr);
		break;
	case AGGR_DIE:
		fprintf(output, "\"die\" : \"S%d-D%d\", \"counters\" : %d, ", id.socket, id.die,
			aggr_nr);
		break;
	case AGGR_SOCKET:
		fprintf(output, "\"socket\" : \"S%d\", \"counters\" : %d, ", id.socket, aggr_nr);
		break;
	case AGGR_NODE:
		fprintf(output, "\"node\" : \"N%d\", \"counters\" : %d, ", id.node, aggr_nr);
		break;
	case AGGR_NONE:
		if (evsel->percore && !config->percore_show_thread)
			fprintf(output, "\"core\" : \"S%d-D%d-C%d\", ", id.socket, id.die, id.core);
		else if (id.cpu.cpu > -1)
			fprintf(output, "\"cpu\" : \"%d\", ", id.cpu.cpu);
		break;
	case AGGR_THREAD:
		fprintf(output, "\"thread\" : \"%s-%d\", ",
			perf_thread_map__comm(evsel->core.threads, id.thread_idx),
			perf_thread_map__pid(evsel->core.threads, id.thread_idx));
		break;
	case AGGR_GLOBAL:
	case AGGR_UNSET:
	case AGGR_MAX:
	default:
		break;
	}
}

/*
 * JSON Output Callbacks - Normal Mode (100% Streaming & Zero-Allocation)
 */

static int json_print_start(void *ctx __maybe_unused,
			    const struct perf_stat_config *config __maybe_unused)
{
	return 0;
}

static int json_print_event(void *ctx, const struct perf_stat_config *config, struct evsel *evsel,
			    int aggr_idx, u64 val, u64 ena, u64 run,
			    double stdev_pct __maybe_unused)
{
	struct json_print_state *ps = ctx;
	FILE *output = config->output;
	bool ok = (run != 0 && ena != 0);
	double enabled_percent = 100.0;

	fprintf(output, "{");

	/* Print interval timestamp first if configured */
	if (config->interval && ps && ps->timestamp[0])
		fprintf(output, "%s", ps->timestamp);

	/* Print aggregation JSON fields if configured */
	if (config->aggr_map && aggr_idx >= 0) {
		struct aggr_cpu_id id = config->aggr_map->map[aggr_idx];
		int aggr_nr = 0;

		if (evsel->stats && evsel->stats->aggr)
			aggr_nr = evsel->stats->aggr[aggr_idx].nr;

		print_aggr_id_json(config, output, evsel, id, aggr_nr);
	}

	if (ok) {
		double sc = evsel->scale;
		double avg = val * sc;

		fprintf(output, "\"counter-value\" : \"%f\"", avg);
	} else {
		const char *bad_count = evsel->supported ? CNTR_NOT_COUNTED : CNTR_NOT_SUPPORTED;

		fprintf(output, "\"counter-value\" : \"%s\"", bad_count);
	}

	fprintf(output, ", \"unit\" : \"%s\"", evsel->unit ?: "");
	/* Cast away const for legacy evsel__name */
	fprintf(output, ", \"event\" : \"%s\"", evsel__name((struct evsel *)evsel));

	if (run != ena)
		enabled_percent = 100.0 * run / ena;
	fprintf(output, ", \"event-runtime\" : %" PRIu64 ", \"pcnt-running\" : %.2f", run,
		enabled_percent);
	fprintf(output, "}\n");

	return 0;
}

static int json_print_metric(void *ctx, const struct perf_stat_config *config, struct evsel *evsel,
			     int aggr_idx, const char *name, const char *unit __maybe_unused,
			     double val, enum metric_threshold_classify thresh)
{
	struct json_print_state *ps = ctx;
	FILE *output = config->output;
	u64 run = 0, ena = 0;
	double enabled_percent = 100.0;
	struct perf_stat_evsel *ps_evsel = evsel->stats;

	if (ps_evsel && ps_evsel->aggr) {
		run = ps_evsel->aggr[aggr_idx].counts.run;
		ena = ps_evsel->aggr[aggr_idx].counts.ena;
	}

	fprintf(output, "{");

	/* Print interval timestamp first if configured */
	if (config->interval && ps && ps->timestamp[0])
		fprintf(output, "%s", ps->timestamp);

	/* Print aggregation JSON fields if configured */
	if (config->aggr_map && aggr_idx >= 0) {
		struct aggr_cpu_id id = config->aggr_map->map[aggr_idx];
		int aggr_nr = 0;

		if (evsel->stats && evsel->stats->aggr)
			aggr_nr = evsel->stats->aggr[aggr_idx].nr;

		print_aggr_id_json(config, output, evsel, id, aggr_nr);
	}

	if (run != ena)
		enabled_percent = 100.0 * run / ena;
	fprintf(output, "\"event-runtime\" : %" PRIu64 ", \"pcnt-running\" : %.2f", run,
		enabled_percent);
	fprintf(output, ", \"metric-value\" : \"%f\"", val);
	if (name && name[0])
		fprintf(output, ", \"metric-unit\" : \"%s\"", name);
	if (thresh != METRIC_THRESHOLD_UNKNOWN) {
		fprintf(output, ", \"metric-threshold\" : \"%s\"",
			metric_threshold_classify__str(thresh));
	}
	fprintf(output, "}\n");

	return 0;
}

static int json_print_end(void *ctx __maybe_unused,
			  const struct perf_stat_config *config __maybe_unused)
{
	return 0;
}

static const struct perf_stat_print_callbacks json_print_callbacks = {
	.print_start = json_print_start,
	.print_end = json_print_end,
	.print_event = json_print_event,
	.print_metric = json_print_metric,
};

/*
 * JSON Output Callbacks - Metric-Only Mode (100% Streaming & Zero-Allocation)
 */

static int json_metric_only_print_start(void *ctx,
					const struct perf_stat_config *config __maybe_unused)
{
	struct json_metric_only_print_state *ps = ctx;

	/* Initialize to -2 to distinguish from -1 (a valid index in AGGR_GLOBAL mode) */
	ps->last_aggr_idx = -2;
	ps->first_in_group = true;
	return 0;
}

static int json_metric_only_print_metric(void *ctx,
					 const struct perf_stat_config *config __maybe_unused,
					 struct evsel *evsel __maybe_unused, int aggr_idx,
					 const char *name, const char *unit, double val,
					 enum metric_threshold_classify thresh __maybe_unused)
{
	struct json_metric_only_print_state *ps = ctx;
	FILE *output = ps->fp;


	if (aggr_idx != ps->last_aggr_idx) {
		if (ps->last_aggr_idx != -2)
			fprintf(output, "}\n");
		fprintf(output, "{");
		if (config->interval && ps->timestamp[0])
			fprintf(output, "%s", ps->timestamp);
		if (config->aggr_map && aggr_idx >= 0) {
			struct aggr_cpu_id id = config->aggr_map->map[aggr_idx];
			struct evsel *mock_evsel = list_first_entry(&ps->evlist->core.entries,
								    struct evsel, core.node);
			int aggr_nr = 0;

			if (mock_evsel->stats && mock_evsel->stats->aggr)
				aggr_nr = mock_evsel->stats->aggr[aggr_idx].nr;

			print_aggr_id_json(config, output, mock_evsel, id, aggr_nr);
		}
		ps->last_aggr_idx = aggr_idx;
		ps->first_in_group = true;
	}

	if (!ps->first_in_group)
		fprintf(output, ", ");
	ps->first_in_group = false;

	if (unit && unit[0])
		fprintf(output, "\"%s  %s\" : \"%.1f\"", unit, name, val);
	else
		fprintf(output, "\"%s\" : \"%.1f\"", name, val);
	return 0;
}

static int json_metric_only_print_end(void *ctx,
				      const struct perf_stat_config *config __maybe_unused)
{
	struct json_metric_only_print_state *ps = ctx;
	FILE *output = ps->fp;

	if (ps->last_aggr_idx != -2)
		fprintf(output, "}\n");
	return 0;
}

static const struct perf_stat_print_callbacks json_metric_only_print_callbacks = {
	.print_start = json_metric_only_print_start,
	.print_end = json_metric_only_print_end,
	.print_event = NULL,
	.print_metric = json_metric_only_print_metric,
};

int perf_stat__print_json(struct evlist *evlist, const struct perf_stat_config *config,
			  const struct target *target, const struct timespec *ts, int argc,
			  const char **argv)
{
	if (config->metric_only) {
		struct json_metric_only_print_state ps = {
			.fp = config->output,
			.evlist = evlist,
		};
		if (config->interval && ts) {
			scnprintf(ps.timestamp, sizeof(ps.timestamp), "\"interval\" : %lu.%09lu, ",
				  (unsigned long)ts->tv_sec, ts->tv_nsec);
		} else {
			ps.timestamp[0] = '\0';
		}
		return perf_stat__print_cb(evlist, config, target, ts, argc, argv,
					   &json_metric_only_print_callbacks, &ps);
	} else {
		struct json_print_state ps = {
			.fp = config->output,
		};
		if (config->interval && ts) {
			scnprintf(ps.timestamp, sizeof(ps.timestamp), "\"interval\" : %lu.%09lu, ",
				  (unsigned long)ts->tv_sec, ts->tv_nsec);
		} else {
			ps.timestamp[0] = '\0';
		}
		return perf_stat__print_cb(evlist, config, target, ts, argc, argv,
					   &json_print_callbacks, &ps);
	}
}
