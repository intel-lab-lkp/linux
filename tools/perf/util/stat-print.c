// SPDX-License-Identifier: GPL-2.0
#include "stat-print.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/compiler.h>

#include "cpumap.h"
#include "debug.h"
#include "evlist.h"
#include "evsel.h"
#include "expr.h"
#include "metricgroup.h"
#include "stat.h"
#include "thread_map.h"
#include "tool_pmu.h"

/*
 * Unified Aggregation Helpers (Shared by STD, CSV, JSON Formats)
 */

const char *perf_stat__get_aggr_key(const struct perf_stat_config *config,
				    const struct evsel *evsel)
{
	switch (config->aggr_mode) {
	case AGGR_CORE:
		return "core";
	case AGGR_CACHE:
		return "cache";
	case AGGR_CLUSTER:
		return "cluster";
	case AGGR_DIE:
		return "die";
	case AGGR_SOCKET:
		return "socket";
	case AGGR_NODE:
		return "node";
	case AGGR_NONE:
		if (evsel->percore && !config->percore_show_thread)
			return "core";
		return "cpu";
	case AGGR_THREAD:
		return "thread";
	case AGGR_GLOBAL:
	case AGGR_UNSET:
	case AGGR_MAX:
	default:
		return "";
	}
}

int perf_stat__get_aggr_id_char(const struct perf_stat_config *config, struct evsel *evsel,
				struct aggr_cpu_id id, char *buf, size_t buf_size)
{
	switch (config->aggr_mode) {
	case AGGR_CORE:
		return scnprintf(buf, buf_size, "S%d-D%d-C%d", id.socket, id.die, id.core);
	case AGGR_CACHE:
		return scnprintf(buf, buf_size, "S%d-D%d-L%d-ID%d", id.socket, id.die, id.cache_lvl,
				 id.cache);
	case AGGR_CLUSTER:
		return scnprintf(buf, buf_size, "S%d-D%d-CLS%d", id.socket, id.die, id.cluster);
	case AGGR_DIE:
		return scnprintf(buf, buf_size, "S%d-D%d", id.socket, id.die);
	case AGGR_SOCKET:
		return scnprintf(buf, buf_size, "S%d", id.socket);
	case AGGR_NODE:
		return scnprintf(buf, buf_size, "N%d", id.node);
	case AGGR_NONE:
		if (evsel->percore && !config->percore_show_thread) {
			return scnprintf(buf, buf_size, "S%d-D%d-C%d", id.socket, id.die, id.core);
		} else if (id.cpu.cpu > -1) {
			return scnprintf(buf, buf_size, "%d", id.cpu.cpu);
		}
		break;
	case AGGR_THREAD:
		return scnprintf(buf, buf_size, "%s-%d",
				 perf_thread_map__comm(evsel->core.threads, id.thread_idx),
				 perf_thread_map__pid(evsel->core.threads, id.thread_idx));
	case AGGR_GLOBAL:
	case AGGR_UNSET:
	case AGGR_MAX:
	default:
		break;
	}
	buf[0] = '\0';
	return -1;
}

/*
 * Traversal Driver and Calculation Code
 */

/**
 * tool_pmu__is_time_event - Check if event is a tool PMU time event.
 *
 * Copied from stat-shadow.c to make stat-print.c self-contained.
 */
static bool tool_pmu__is_time_event(const struct perf_stat_config *config,
				    const struct evsel *evsel, int *tool_aggr_idx)
{
	enum tool_pmu_event event = evsel__tool_event(evsel);
	int aggr_idx;

	if (event != TOOL_PMU__EVENT_DURATION_TIME && event != TOOL_PMU__EVENT_USER_TIME &&
	    event != TOOL_PMU__EVENT_SYSTEM_TIME)
		return false;

	if (config) {
		cpu_aggr_map__for_each_idx(aggr_idx, config->aggr_map) {
			if (config->aggr_map->map[aggr_idx].cpu.cpu == 0) {
				*tool_aggr_idx = aggr_idx;
				return true;
			}
		}
		pr_debug("Unexpected CPU0 missing in aggregation for tool event.\n");
	}
	*tool_aggr_idx = 0; /* Assume the first aggregation index works. */
	return true;
}

/**
 * prepare_metric - Collect event values required for a metric.
 * @config: Perf stat configuration.
 * @mexp: The metric expression.
 * @evsel: The associated event selector.
 * @pctx: Expr parse context to add ID/values to.
 * @aggr_idx: Aggregation index to read values from.
 *
 * Iterates over the events required for the metric expression, reads their
 * counts for the given aggregation index, and adds them to the expression
 * parser context.
 *
 * Copied and refactored from stat-shadow.c.
 */
static int prepare_metric(const struct perf_stat_config *config, const struct metric_expr *mexp,
			  struct evsel *evsel, struct expr_parse_ctx *pctx, int aggr_idx)
{
	struct evsel *const *metric_events = mexp->metric_events;
	struct metric_ref *metric_refs = mexp->metric_refs;
	int i;

	for (i = 0; metric_events[i]; i++) {
		int source_count = 0, tool_aggr_idx;
		bool is_tool_time =
			tool_pmu__is_time_event(config, metric_events[i], &tool_aggr_idx);
		struct perf_stat_evsel *ps = metric_events[i]->stats;
		char *n;
		double val;

		/*
		 * If there are multiple uncore PMUs and we're not reading the
		 * leader's stats, determine the stats for the appropriate
		 * uncore PMU.
		 */
		if (evsel && evsel->metric_leader && evsel->pmu != evsel->metric_leader->pmu &&
		    mexp->metric_events[i]->pmu == evsel->metric_leader->pmu) {
			struct evsel *pos;

			evlist__for_each_entry(evsel->evlist, pos) {
				if (pos->pmu != evsel->pmu)
					continue;
				if (pos->metric_leader != mexp->metric_events[i])
					continue;
				ps = pos->stats;
				source_count = 1;
				break;
			}
		}
		/* Time events are always on CPU0, the first aggregation index. */
		if (!ps || !metric_events[i]->supported) {
			val = NAN;
			source_count = 0;
		} else {
			struct perf_stat_aggr *aggr =
				&ps->aggr[is_tool_time ? tool_aggr_idx : aggr_idx];

			if (aggr->counts.run == 0) {
				val = NAN;
				source_count = 0;
			} else {
				val = aggr->counts.val;
				if (is_tool_time) {
					/* Convert time event nanoseconds to seconds. */
					val *= 1e-9;
				}
				if (!source_count)
					source_count = evsel__source_count(metric_events[i]);
			}
		}
		n = strdup(evsel__metric_id(metric_events[i]));
		if (!n)
			return -ENOMEM;

		expr__add_id_val_source_count(pctx, n, val, source_count);
	}

	for (int j = 0; metric_refs && metric_refs[j].metric_name; j++) {
		int ret = expr__add_ref(pctx, &metric_refs[j]);

		if (ret)
			return ret;
	}

	return i;
}

/**
 * calculate_and_print_metric - Compute and print a single metric.
 *
 * Parses the metric expression, computes the ratio, and calls the print_metric
 * callback directly with clean parameters.
 * Returns the return value of the print_metric callback (0 on success, or error).
 */
static int calculate_and_print_metric(const struct perf_stat_config *config,
				      const struct perf_stat_print_callbacks *cb, void *outer_ctx,
				      struct metric_expr *mexp, struct evsel *evsel, int aggr_idx)
{
	const char *metric_name = mexp->metric_name;
	const char *metric_expr = mexp->metric_expr;
	const char *metric_threshold = mexp->metric_threshold;
	const char *metric_unit = mexp->metric_unit;
	struct evsel *const *metric_events = mexp->metric_events;
	int runtime = mexp->runtime;
	struct expr_parse_ctx *pctx;
	double ratio, scale, threshold;
	int i;
	enum metric_threshold_classify thresh = METRIC_THRESHOLD_UNKNOWN;
	int ret = 0;

	if (!cb->print_metric)
		return 0;

	pctx = expr__ctx_new();
	if (!pctx)
		return -ENOMEM;

	if (config->user_requested_cpu_list)
		pctx->sctx.user_requested_cpu_list = strdup(config->user_requested_cpu_list);
	pctx->sctx.runtime = runtime;
	pctx->sctx.system_wide = config->system_wide;
	i = prepare_metric(config, mexp, evsel, pctx, aggr_idx);
	if (i < 0) {
		expr__ctx_free(pctx);
		return i;
	}
	if (!metric_events[i]) {
		if (expr__parse(&ratio, pctx, metric_expr) == 0) {
			char *unit;

			if (metric_threshold &&
			    expr__parse(&threshold, pctx, metric_threshold) == 0 &&
			    !isnan(threshold)) {
				thresh = fpclassify(threshold) == FP_ZERO ? METRIC_THRESHOLD_GOOD :
									    METRIC_THRESHOLD_BAD;
			}

			if (metric_unit && metric_name) {
				if (perf_pmu__convert_scale(metric_unit, &unit, &scale) >= 0) {
					ratio *= scale;
				}
				ret = cb->print_metric(outer_ctx, config, evsel, aggr_idx,
						       metric_name, unit, ratio, thresh);
			} else {
				ret = cb->print_metric(outer_ctx, config, evsel, aggr_idx,
						       metric_name ?: (evsel->name ?: ""), NULL,
						       ratio, thresh);
			}
		}
	}

	expr__ctx_free(pctx);
	return ret;
}

/**
 * perf_stat_print_metricgroup - Traverse metrics for an event.
 *
 * Returns 0 on success, or a negative error code on failure.
 */
static bool is_basic_shadow_metric(const char *name)
{
	static const char *const basic_metrics[] = {
		"insn_per_cycle",   "branch_miss_rate",	      "branch_frequency",
		"cycles_frequency", "page_faults_per_second", "migrations_per_second",
		"cs_per_second",    "CPUs_utilized",
	};
	for (size_t i = 0; i < ARRAY_SIZE(basic_metrics); i++) {
		if (!strcmp(basic_metrics[i], name))
			return true;
	}
	return false;
}

static int perf_stat_print_metricgroup(const struct perf_stat_config *config,
				       const struct perf_stat_print_callbacks *cb, void *outer_ctx,
				       struct evsel *evsel, int aggr_idx)
{
	struct metric_event *me;
	struct metric_expr *mexp;
	struct rblist *metric_events = &evsel->evlist->metric_events;
	int ret;

	me = metricgroup__lookup(metric_events, evsel, false);
	if (me == NULL)
		return 0;

	list_for_each_entry(mexp, &me->head, nd) {
		if (!config->metric_only &&
		    (!evsel->default_metricgroup || evsel->default_show_events)) {
			if (!is_basic_shadow_metric(mexp->metric_name))
				continue;
		}

		ret = calculate_and_print_metric(config, cb, outer_ctx, mexp, evsel, aggr_idx);
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * perf_stat_print_metrics - Entry point for metric calculation & printing.
 *
 * Returns 0 on success, or a negative error code on failure.
 */
static int perf_stat_print_metrics(const struct perf_stat_config *config,
				   const struct perf_stat_print_callbacks *cb, void *outer_ctx,
				   struct evsel *evsel, int aggr_idx)
{
	if (config->iostat_run) {
		/* IOSTAT metrics not supported yet in new API */
		return 0;
	}

	return perf_stat_print_metricgroup(config, cb, outer_ctx, evsel, aggr_idx);
}

int perf_stat__print_cb(struct evlist *evlist, const struct perf_stat_config *config,
			const struct target *target __maybe_unused,
			const struct timespec *ts __maybe_unused, int argc __maybe_unused,
			const char **argv __maybe_unused,
			const struct perf_stat_print_callbacks *cb, void *ctx)
{
	struct evsel *counter;
	int aggr_idx;
	int ret = 0;

	evlist__uniquify_evsel_names(evlist, config);

	if (cb->print_start) {
		ret = cb->print_start(ctx, config);
		if (ret)
			return ret;
	}

	switch (config->aggr_mode) {
	case AGGR_GLOBAL:
	case AGGR_NONE:
	case AGGR_SOCKET:
	case AGGR_DIE:
	case AGGR_CLUSTER:
	case AGGR_CACHE:
	case AGGR_CORE:
	case AGGR_THREAD:
	case AGGR_NODE:
		if (config->aggr_map) {
			cpu_aggr_map__for_each_idx(aggr_idx, config->aggr_map) {
				evlist__for_each_entry(evlist, counter) {
					struct perf_stat_evsel *ps = counter->stats;
					u64 val = 0, ena = 0, run = 0;

					if (ps && ps->aggr) {
						val = ps->aggr[aggr_idx].counts.val;
						ena = ps->aggr[aggr_idx].counts.ena;
						run = ps->aggr[aggr_idx].counts.run;
					}

					/* Skip already merged uncore/hybrid events */
					if (config->aggr_mode != AGGR_NONE) {
						if (evsel__is_hybrid(counter)) {
							if (config->hybrid_merge &&
							    counter->first_wildcard_match != NULL)
								continue;
						} else {
							if (counter->first_wildcard_match != NULL)
								continue;
						}
					}

					if (perf_stat__skip_metric_event(counter))
						continue;

					if (cb->print_event) {
						double stdev_pct = 0.0;
						if (ps && ps->res_stats.n > 1) {
							stdev_pct = rel_stddev_stats(
								stddev_stats(&ps->res_stats), val);
						}
						ret = cb->print_event(ctx, config, counter,
								      aggr_idx, val, ena, run,
								      stdev_pct);
						if (ret)
							goto out;
					}

					ret = perf_stat_print_metrics(config, cb, ctx, counter,
								      aggr_idx);
					if (ret)
						goto out;
				}
			}
		} else {
			evlist__for_each_entry(evlist, counter) {
				struct perf_stat_evsel *ps = counter->stats;
				u64 val = 0, ena = 0, run = 0;

				if (ps && ps->aggr) {
					val = ps->aggr[0].counts.val;
					ena = ps->aggr[0].counts.ena;
					run = ps->aggr[0].counts.run;
				}

				/* Skip already merged uncore/hybrid events */
				if (config->aggr_mode != AGGR_NONE) {
					if (evsel__is_hybrid(counter)) {
						if (config->hybrid_merge &&
						    counter->first_wildcard_match != NULL)
							continue;
					} else {
						if (counter->first_wildcard_match != NULL)
							continue;
					}
				}

				if (perf_stat__skip_metric_event(counter))
					continue;

				if (cb->print_event) {
					double stdev_pct = 0.0;
					if (ps && ps->res_stats.n > 1) {
						stdev_pct = rel_stddev_stats(
							stddev_stats(&ps->res_stats), val);
					}
					ret = cb->print_event(ctx, config, counter, 0, val, ena,
							      run, stdev_pct);
					if (ret)
						goto out;
				}

				ret = perf_stat_print_metrics(config, cb, ctx, counter, 0);
				if (ret)
					goto out;
			}
		}
		break;
	case AGGR_UNSET:
	case AGGR_MAX:
	default:
		fprintf(config->output, "Aggregation mode %d not supported in new API yet\n",
			config->aggr_mode);
		break;
	}

out:
	if (cb->print_end) {
		int err = cb->print_end(ctx, config);
		if (!ret)
			ret = err;
	}

	return ret;
}

int perf_stat__print(struct evlist *evlist, const struct perf_stat_config *config,
		     const struct target *target, const struct timespec *ts, int argc,
		     const char **argv)
{
	if (config->csv_output) {
		return perf_stat__print_csv(evlist, config, target, ts, argc, argv);
	} else if (config->json_output) {
		return perf_stat__print_json(evlist, config, target, ts, argc, argv);
	} else {
		return perf_stat__print_std(evlist, config, target, ts, argc, argv);
	}
}
