// SPDX-License-Identifier: GPL-2.0
#include "linux/string.h"
#include "util/map_symbol.h"
#include "util/mem-events.h"
#include "mem-events.h"


#define MEM_LOADS_AUX		0x8203

#define E(t, n, s, l, a) { .tag = t, .name = n, .event_name = s, .ldlat = l, .aux_event = a }

struct perf_mem_event perf_mem_events_intel[PERF_MEM_EVENTS__MAX] = {
	E("ldlat-loads",	"%s/mem-loads,ldlat=%u/P",	"mem-loads",	true,	0),
	E("ldlat-stores",	"%s/mem-stores/P",		"mem-stores",	false,	0),
	E(NULL,			NULL,				NULL,		false,	0),
};

struct perf_mem_event perf_mem_events_intel_aux[PERF_MEM_EVENTS__MAX] = {
	E("ldlat-loads",	"{%s/mem-loads-aux/,%s/mem-loads,ldlat=%u/}:P",	"mem-loads",	true,	MEM_LOADS_AUX),
	E("ldlat-stores",	"%s/mem-stores/P",		"mem-stores",	false,	0),
	E(NULL,			NULL,				NULL,		false,	0),
};

/*
 * IBS events with exclude_{user,kernel} bits set, as used by perf to
 * record per-thread when kernel samples are not allowed, are rejected
 * by the kernel on hardware without the privilege filter unless the
 * swfilt software filter is used, so there are extra sets of tables
 * with the swfilt term in the event names, selected in
 * perf_pmu__arch_init() when the PMU exposes the term.  The term is not
 * conditional on the event already having exclude bits: perf record
 * adds those bits itself when the first open fails with EACCES on an
 * unprivileged setup, after the name was built, and that retry only
 * succeeds with the term in it.
 */
struct perf_mem_event perf_mem_events_amd[PERF_MEM_EVENTS__MAX] = {
	E(NULL,		NULL,		NULL,	false,	0),
	E(NULL,		NULL,		NULL,	false,	0),
	E("mem-ldst",	"%s//",		NULL,	false,	0),
};

struct perf_mem_event perf_mem_events_amd_swfilt[PERF_MEM_EVENTS__MAX] = {
	E(NULL,		NULL,		NULL,	false,	0),
	E(NULL,		NULL,		NULL,	false,	0),
	E("mem-ldst",	"%s/swfilt=1/",	NULL,	false,	0),
};

struct perf_mem_event perf_mem_events_amd_ldlat[PERF_MEM_EVENTS__MAX] = {
	E(NULL,		NULL,		NULL,	false,	0),
	E(NULL,		NULL,		NULL,	false,	0),
	E("mem-ldst",	"%s/ldlat=%u/",	NULL,	true,	0),
};

struct perf_mem_event perf_mem_events_amd_ldlat_swfilt[PERF_MEM_EVENTS__MAX] = {
	E(NULL,		NULL,			NULL,	false,	0),
	E(NULL,		NULL,			NULL,	false,	0),
	E("mem-ldst",	"%s/ldlat=%u,swfilt=1/",	NULL,	true,	0),
};
