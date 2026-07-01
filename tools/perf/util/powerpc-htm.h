/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HTM PMU Support
 */

#ifndef INCLUDE__PERF_POWERPC_HTM_H__
#define INCLUDE__PERF_POWERPC_HTM_H__

#define POWERPC_HTM_NAME "powerpc_htm_"

enum {
	POWERPC_HTM_TYPE,
	HTM_AUXTRACE_PRIV_MAX,
};

#define HTM_AUXTRACE_PRIV_SIZE (HTM_AUXTRACE_PRIV_MAX * sizeof(u64))

union perf_event;
struct perf_session;
struct perf_pmu;

struct auxtrace_record *htm_recording_init(struct evsel *pos);
#endif
