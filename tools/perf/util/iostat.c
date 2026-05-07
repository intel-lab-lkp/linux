// SPDX-License-Identifier: GPL-2.0
#include "util/iostat.h"

/*
 * Below iostat_* function calls are scattered through out perf stat process,
 * allowing multiple iostat PMUs and iterated them in following functions may
 * violate calling conventions or cause incorrect display.
 *
 * Default to register the first PMU device that matches any of the specified
 * iostat pmu name wildcards.
 */
static struct iostat_pmu *iostat_pmu;

enum iostat_mode_t iostat_mode = IOSTAT_NONE;

int iostat_prepare(struct evlist *evlist, struct perf_stat_config *config)
{
	if (!iostat_pmu)
		return -1;

	return iostat_pmu->prepare(evlist, config);
}

int iostat_parse(const struct option *opt, const char *str, int unset)
{
	if (!iostat_pmu)
		return -1;

	return iostat_pmu->parse(opt, str, unset);
}

void iostat_list(struct evlist *evlist, struct perf_stat_config *config)
{
	iostat_pmu->list(evlist, config);
}

void iostat_release(struct evlist *evlist)
{
	iostat_pmu->release(evlist);
}

void iostat_print_header_prefix(struct perf_stat_config *config)
{
	iostat_pmu->print_header_prefix(config);
}

void iostat_print_metric(struct perf_stat_config *config,
			 struct evsel *evsel,
			 struct perf_stat_output_ctx *out)
{
	iostat_pmu->print_metric(config, evsel, out);
}

void iostat_print_counters(struct evlist *evlist,
			   struct perf_stat_config *config,
			   struct timespec *ts, char *prefix,
			   iostat_print_counter_t print_cnt_cb,
			   void *arg)
{
	iostat_pmu->print_counters(evlist, config, ts, prefix,
				   print_cnt_cb, arg);
}

void register_iostat_pmu(struct iostat_pmu *pmu)
{
	if (!pmu || !pmu->match)
		return;

	if (iostat_pmu || !pmu->match(pmu))
		return;

	iostat_pmu = pmu;
}

static void unregister_iostat_pmu(void)
{
	if (!iostat_pmu)
		return;

	/*
	 * Release function of iostat_pmu is called on the exit of cmd_stat, we
	 * don't need to call release function here.
	 */
	iostat_pmu = NULL;
}

__attribute__((destructor))
static void iostat_exit(void)
{
	unregister_iostat_pmu();
}
