/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_STAT_PRINT_H
#define __PERF_STAT_PRINT_H

#include <linux/types.h>

#include "stat.h"

#define CNTR_NOT_SUPPORTED "<not supported>"
#define CNTR_NOT_COUNTED "<not counted>"

struct evlist;
struct perf_stat_config;
struct target;
struct timespec;
struct evsel;
struct aggr_cpu_id;

extern const int aggr_header_lens[];
extern const char *aggr_header_csv[];
extern const char *aggr_header_std[];

/**
 * struct perf_stat_print_callbacks - Callbacks for rendering perf stat output.
 *
 * This structure defines the interface for different output formats (e.g.,
 * Standard, CSV, JSON) to render the collected performance counter statistics.
 * The core display logic traverses the events and metrics and calls these
 * callbacks in a streaming fashion, which build an in-memory DOM tree. The
 * final rendering and output formatting is executed entirely in print_end.
 */
struct perf_stat_print_callbacks {
	/**
	 * print_start - Called before any event or metric is printed.
	 * @ctx: Opaque context pointer passed to the print function.
	 * @config: Perf stat configuration.
	 */
	int (*print_start)(void *ctx, const struct perf_stat_config *config);

	/**
	 * print_end - Called after all events and metrics have been traversed.
	 * Executes the actual formatting and printing of the buffered tree.
	 * @ctx: Opaque context pointer.
	 * @config: Perf stat configuration.
	 */
	int (*print_end)(void *ctx, const struct perf_stat_config *config);

	/**
	 * print_event - Called to buffer an event (counter) value.
	 * @ctx: Opaque context pointer.
	 * @config: Perf stat configuration.
	 * @evsel: The event selector being printed (mutable for lazy initialization).
	 * @aggr_idx: Aggregation index in evsel->stats.
	 * @val: Raw counter value.
	 * @ena: Enabled time for the counter (for multiplexing).
	 * @run: Running time for the counter (for multiplexing).
	 * @stdev_pct: Standard deviation percentage across multiple repeated runs.
	 *
	 * Returns 0 on success, or a negative error code (e.g., -ENOMEM) on failure.
	 */
	int (*print_event)(void *ctx, const struct perf_stat_config *config, struct evsel *evsel,
			   int aggr_idx, u64 val, u64 ena, u64 run, double stdev_pct);

	/**
	 * print_metric - Called to buffer a metric value associated with an event.
	 * @ctx: Opaque context pointer.
	 * @config: Perf stat configuration.
	 * @evsel: The event selector associated with the metric (mutable).
	 * @aggr_idx: Aggregation index.
	 * @name: The display name of the metric.
	 * @unit: The unit of the metric (e.g., "%", "GHz", or NULL).
	 * @val: The calculated metric value.
	 * @thresh: Threshold classification (e.g., good, bad) for color coding.
	 *
	 * Returns 0 on success, or a negative error code (e.g., -ENOMEM) on failure.
	 */
	int (*print_metric)(void *ctx, const struct perf_stat_config *config, struct evsel *evsel,
			    int aggr_idx, const char *name, const char *unit, double val,
			    enum metric_threshold_classify thresh);
};

/**
 * perf_stat__get_aggr_key - Get the JSON key name for an aggregation mode.
 */
const char *perf_stat__get_aggr_key(const struct perf_stat_config *config,
				    const struct evsel *evsel);

/**
 * perf_stat__get_aggr_id_char - Get the unified aggregation ID string.
 *
 * Returns the formatted string size, or a negative error code on failure.
 */
int perf_stat__get_aggr_id_char(const struct perf_stat_config *config, struct evsel *evsel,
				struct aggr_cpu_id id, char *buf, size_t buf_size);

/**
 * perf_stat__print_cb - Drive the traversal and call callbacks.
 *
 * Defined in stat-print.c. Called by format-specific entry points.
 * Returns 0 on success, or a negative error code on failure.
 */
int perf_stat__print_cb(struct evlist *evlist, const struct perf_stat_config *config,
			const struct target *target, const struct timespec *ts, int argc,
			const char **argv, const struct perf_stat_print_callbacks *cb, void *ctx);

/**
 * perf_stat__print - Entry point for the decoupled print API.
 *
 * Defined in stat-print.c. Dispatches to format-specific entry points.
 * Returns 0 on success, or a negative error code on failure.
 */
int perf_stat__print(struct evlist *evlist, const struct perf_stat_config *config,
		     const struct target *target, const struct timespec *ts, int argc,
		     const char **argv);

/*
 * Format-specific entry points, implemented in their respective files.
 * All return 0 on success, or a negative error code on failure.
 */

int perf_stat__print_std(struct evlist *evlist, const struct perf_stat_config *config,
			 const struct target *target, const struct timespec *ts, int argc,
			 const char **argv);

int perf_stat__print_csv(struct evlist *evlist, const struct perf_stat_config *config,
			 const struct target *target, const struct timespec *ts, int argc,
			 const char **argv);

int perf_stat__print_json(struct evlist *evlist, const struct perf_stat_config *config,
			  const struct target *target, const struct timespec *ts, int argc,
			  const char **argv);

#endif /* __PERF_STAT_PRINT_H */
