/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __POWERPC_HTM_H
#define __POWERPC_HTM_H

#include <linux/types.h>

/*
 * Layout of the private data in PERF_RECORD_AUXTRACE_INFO for HTM.
 *
 * priv[POWERPC_HTM_PMU_TYPE]   = htm PMU type ID (pmu->type from kernel)
 * priv[POWERPC_HTM_NUM_EVENTS] = number of htm evsels recorded (N)
 *
 * Followed by N pairs (2 u64 each):
 *   priv[POWERPC_HTM_EVENT_DATA + n*2 + 0] = CPU number for nth htm evsel
 *   priv[POWERPC_HTM_EVENT_DATA + n*2 + 1] = attr.config for nth htm evsel
 *
 * Total priv entries: POWERPC_HTM_EVENT_DATA + N * 2
 */
enum {
	POWERPC_HTM_PMU_TYPE  = 0,
	POWERPC_HTM_NUM_EVENTS,
	POWERPC_HTM_EVENT_DATA,	/* variable-length: 2 u64 per event */
};

/* Fixed header size (everything before the per-event data) */
#define HTM_AUXTRACE_PRIV_FIXED  (POWERPC_HTM_EVENT_DATA * sizeof(u64))

/* Total priv size for N htm evsels */
#define HTM_AUXTRACE_PRIV_SIZE(n) \
	(HTM_AUXTRACE_PRIV_FIXED + (n) * 2 * sizeof(u64))

struct evsel;
struct evlist;
union perf_event;
struct perf_session;
struct auxtrace_record;

struct auxtrace_record *htm_recording_init(struct evsel *pos, int *err);

int powerpc_htm_process_auxtrace_info(union perf_event *event,
				      struct perf_session *session);

#endif /* __POWERPC_HTM_H */
