/* SPDX-License-Identifier: GPL-2.0 */
/*
 * perf iostat
 *
 * Copyright (C) 2020, Intel Corporation
 *
 * Authors: Alexander Antonov <alexander.antonov@linux.intel.com>
 */

#ifndef _IOSTAT_H
#define _IOSTAT_H

#include <stdbool.h>
#include <subcmd/parse-options.h>
#include "util/stat.h"
#include "util/parse-events.h"
#include "util/evlist.h"

struct option;
struct perf_stat_config;
struct evlist;
struct timespec;

enum iostat_mode_t {
	IOSTAT_NONE		= -1,
	IOSTAT_RUN		= 0,
	IOSTAT_LIST		= 1
};

extern enum iostat_mode_t iostat_mode;

typedef void (*iostat_print_counter_t)(struct perf_stat_config *, struct evsel *, void *);

int iostat_prepare(struct evlist *evlist, struct perf_stat_config *config);
int iostat_parse(const struct option *opt, const char *str, int unset);
void iostat_list(struct evlist *evlist, struct perf_stat_config *config);
void iostat_release(struct evlist *evlist);
void iostat_print_header_prefix(struct perf_stat_config *config);
void iostat_print_metric(struct perf_stat_config *config, struct evsel *evsel,
			 struct perf_stat_output_ctx *out);
void iostat_print_counters(struct evlist *evlist,
			   struct perf_stat_config *config, struct timespec *ts,
			   char *prefix, iostat_print_counter_t print_cnt_cb, void *arg);

/**
 * struct iostat_pmu - Callbacks for an iostat-capable PMU backend.
 * @pmu_name_wildcard: Glob pattern to identify the PMU (e.g. "uncore_iio*").
 * @match: Detect whether matching PMUs exist on this system.
 * @prepare: Set up events and config for iostat collection.
 * @parse: Parse the --iostat option argument.
 * @list: Display available iostat PMU instances.
 * @print_header_prefix: Print the column header prefix.
 * @print_metric: Format and print one metric value.
 * @print_counters:  Iterate over counters and print per-port results.
 * @release: Clean up PMU-specific resources.
 */
struct iostat_pmu {
	const char *pmu_name_wildcard;
	bool (*match)(struct iostat_pmu *iostat_pmu);
	int (*prepare)(struct evlist *evlist, struct perf_stat_config *config);
	int (*parse)(const struct option *opt, const char *str, int unset);
	void (*list)(struct evlist *evlist, struct perf_stat_config *config);
	void (*print_header_prefix)(struct perf_stat_config *config);
	void (*print_metric)(struct perf_stat_config *config, struct evsel *evsel,
			     struct perf_stat_output_ctx *out);
	void (*print_counters)(struct evlist *evlist,
			       struct perf_stat_config *config, struct timespec *ts,
			       char *prefix, iostat_print_counter_t print_cnt_cb, void *arg);
	void (*release)(struct evlist *evlist __maybe_unused);
};

/*
 * Register an iostat PMU handler. Called from __attribute__((constructor))
 * functions in each backend's translation unit.
 *
 * Only the first matched backend is activated.
 */
void register_iostat_pmu(struct iostat_pmu *iostat_pmu);
#endif /* _IOSTAT_H */
