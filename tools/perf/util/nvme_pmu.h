/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
#ifndef __NVME_PMU_H
#define __NVME_PMU_H

#include "pmu.h"
#include <stdbool.h>
#include <errno.h>

struct list_head;
struct perf_thread_map;
struct evsel;

#ifdef HAVE_LIBNVME_SUPPORT
struct perf_pmu *nvme_pmu__new(struct list_head *pmus, const char *sysfs_name, const char *name);
void nvme_pmu__exit(struct perf_pmu *pmu);

int nvme_pmu__for_each_event(struct perf_pmu *pmu, void *state, pmu_event_callback cb);
size_t nvme_pmu__num_events(struct perf_pmu *pmu);
bool nvme_pmu__have_event(struct perf_pmu *pmu, const char *name);
int nvme_pmu__config_terms(const struct perf_pmu *pmu,
			   struct perf_event_attr *attr,
			   struct parse_events_terms *terms,
			   struct parse_events_error *err);
int nvme_pmu__check_alias(struct parse_events_terms *terms, struct perf_pmu_info *info,
			  struct parse_events_error *err);

bool perf_pmu__is_nvme(const struct perf_pmu *pmu);
bool evsel__is_nvme(const struct evsel *evsel);

int perf_pmus__read_nvme_pmus(struct list_head *pmus);

int evsel__nvme_pmu_open(struct evsel *evsel,
			 struct perf_thread_map *threads,
			 int start_cpu_map_idx, int end_cpu_map_idx);
int evsel__nvme_pmu_read(struct evsel *evsel, int cpu_map_idx, int thread);
#else
static inline struct perf_pmu *nvme_pmu__new(struct list_head *pmus __maybe_unused,
					     const char *sysfs_name __maybe_unused,
					     const char *name __maybe_unused)
{
	return NULL;
}

static inline void nvme_pmu__exit(struct perf_pmu *pmu __maybe_unused)
{
}

static inline int nvme_pmu__for_each_event(struct perf_pmu *pmu __maybe_unused,
					   void *state __maybe_unused,
					   pmu_event_callback cb __maybe_unused)
{
	return 0;
}

static inline size_t nvme_pmu__num_events(struct perf_pmu *pmu __maybe_unused)
{
	return 0;
}

static inline bool nvme_pmu__have_event(struct perf_pmu *pmu __maybe_unused,
					const char *name __maybe_unused)
{
	return false;
}

static inline int nvme_pmu__config_terms(const struct perf_pmu *pmu __maybe_unused,
					 struct perf_event_attr *attr __maybe_unused,
					 struct parse_events_terms *terms __maybe_unused,
					 struct parse_events_error *err __maybe_unused)
{
	return -EINVAL;
}

static inline int nvme_pmu__check_alias(struct parse_events_terms *terms __maybe_unused,
					struct perf_pmu_info *info __maybe_unused,
					struct parse_events_error *err __maybe_unused)
{
	return -EINVAL;
}

static inline bool perf_pmu__is_nvme(const struct perf_pmu *pmu __maybe_unused)
{
	return false;
}

static inline bool evsel__is_nvme(const struct evsel *evsel __maybe_unused)
{
	return false;
}

static inline int perf_pmus__read_nvme_pmus(struct list_head *pmus __maybe_unused)
{
	return 0;
}

static inline int evsel__nvme_pmu_open(struct evsel *evsel __maybe_unused,
				       struct perf_thread_map *threads __maybe_unused,
				       int start_cpu_map_idx __maybe_unused,
				       int end_cpu_map_idx __maybe_unused)
{
	return 0;
}

static inline int evsel__nvme_pmu_read(struct evsel *evsel __maybe_unused,
				       int cpu_map_idx __maybe_unused,
				       int thread __maybe_unused)
{
	return 0;
}
#endif

#endif /* __NVME_PMU_H */
