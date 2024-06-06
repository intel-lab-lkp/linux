/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_EVSEL_H
#define __LIBPERF_INTERNAL_EVSEL_H

#include <linux/types.h>
#include <linux/perf_event.h>
#include <stdbool.h>
#include <sys/types.h>
#include <internal/cpumap.h>

struct perf_thread_map;
struct xyarray;

/**
 * The per-thread accumulated period storage node.
 */
struct perf_sample_id_period {
	struct list_head	node;
	struct hlist_node	hnode;
	/* Holds total ID period value for PERF_SAMPLE_READ processing. */
	u64			period;
	/* The TID that the values belongs to */
	u32			tid;
};

/**
 * perf_evsel_for_each_per_thread_period_safe - safely iterate thru all the
 * per_stream_periods
 * @evlist:perf_evsel instance to iterate
 * @item: struct perf_sample_id_period iterator
 * @tmp: struct perf_sample_id_period temp iterator
 */
#define perf_evsel_for_each_per_thread_period_safe(evsel, tmp, item) \
	list_for_each_entry_safe(item, tmp, &(evsel)->per_stream_periods, node)


#define PERF_SAMPLE_ID__HLIST_BITS 4
#define PERF_SAMPLE_ID__HLIST_SIZE (1 << PERF_SAMPLE_ID__HLIST_BITS)

/*
 * Per fd, to map back from PERF_SAMPLE_ID to evsel, only used when there are
 * more than one entry in the evlist.
 */
struct perf_sample_id {
	struct hlist_node	 node;
	u64			 id;
	struct perf_evsel	*evsel;
       /*
	* 'idx' will be used for AUX area sampling. A sample will have AUX area
	* data that will be queued for decoding, where there are separate
	* queues for each CPU (per-cpu tracing) or task (per-thread tracing).
	* The sample ID can be used to lookup 'idx' which is effectively the
	* queue number.
	*/
	int			 idx;
	struct perf_cpu		 cpu;
	pid_t			 tid;

	/* Guest machine pid and VCPU, valid only if machine_pid is non-zero */
	pid_t			 machine_pid;
	struct perf_cpu		 vcpu;

	union {
		/*
		 * Holds total ID period value for PERF_SAMPLE_READ processing
		 * (when period is not per-thread).
		 */
		u64			period;
		/*
		 * Holds total ID period value for PERF_SAMPLE_READ processing
		 * (when period is per-thread).
		 */
		struct hlist_head	periods[PERF_SAMPLE_ID__HLIST_SIZE];
	};

	/*
	 * When inherit is combined with PERF_SAMPLE_READ, the period value is
	 * per (id, thread) tuple, rather than per id, so use the stream_id to
	 * uniquely identify the period, rather than the id.
	 */
	bool			 period_per_thread;
};

struct perf_evsel {
	struct list_head	 node;
	struct perf_event_attr	 attr;
	/** The commonly used cpu map of CPUs the event should be opened upon, etc. */
	struct perf_cpu_map	*cpus;
	/**
	 * The cpu map read from the PMU. For core PMUs this is the list of all
	 * CPUs the event can be opened upon. For other PMUs this is the default
	 * cpu map for opening the event on, for example, the first CPU on a
	 * socket for an uncore event.
	 */
	struct perf_cpu_map	*own_cpus;
	struct perf_thread_map	*threads;
	struct xyarray		*fd;
	struct xyarray		*mmap;
	struct xyarray		*sample_id;
	u64			*id;
	u32			 ids;
	struct perf_evsel	*leader;

	/* Where period_per_thread is true, stores the per-thread values */
	struct list_head	per_stream_periods;

	/* parse modifier helper */
	int			 nr_members;
	/*
	 * system_wide is for events that need to be on every CPU, irrespective
	 * of user requested CPUs or threads. Tha main example of this is the
	 * dummy event. Map propagation will set cpus for this event to all CPUs
	 * as software PMU events like dummy, have a CPU map that is empty.
	 */
	bool			 system_wide;
	/*
	 * Some events, for example uncore events, require a CPU.
	 * i.e. it cannot be the 'any CPU' value of -1.
	 */
	bool			 requires_cpu;
	/** Is the PMU for the event a core one? Effects the handling of own_cpus. */
	bool			 is_pmu_core;
	int			 idx;
};

void perf_evsel__init(struct perf_evsel *evsel, struct perf_event_attr *attr,
		      int idx);
int perf_evsel__alloc_fd(struct perf_evsel *evsel, int ncpus, int nthreads);
void perf_evsel__close_fd(struct perf_evsel *evsel);
void perf_evsel__free_fd(struct perf_evsel *evsel);
int perf_evsel__read_size(struct perf_evsel *evsel);
int perf_evsel__apply_filter(struct perf_evsel *evsel, const char *filter);

int perf_evsel__alloc_id(struct perf_evsel *evsel, int ncpus, int nthreads);
void perf_evsel__free_id(struct perf_evsel *evsel);

bool perf_evsel__attr_has_per_thread_sample_period(struct perf_evsel *evsel);

u64 *perf_sample_id__get_period_storage(struct perf_sample_id *sid, u32 tid);

#endif /* __LIBPERF_INTERNAL_EVSEL_H */
