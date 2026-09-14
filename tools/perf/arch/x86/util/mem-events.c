// SPDX-License-Identifier: GPL-2.0
#include "linux/string.h"
#include "util/map_symbol.h"
#include "util/mem-events.h"
#include "mem-events.h"


#define MEM_LOADS_AUX		0x8203

#define E_INIT(t, n, s, l, a, sf) {					\
	.tag = t, .name = n, .event_name = s, .swfilt_name = sf,	\
	.ldlat = l, .aux_event = a }
#define E(t, n, s, l, a) E_INIT(t, n, s, l, a, NULL)

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
 * swfilt software filter is used, so these events carry a variant of
 * their names with the swfilt term, used by perf_pmu__mem_events_name()
 * when the kernel exposes the term.
 */
struct perf_mem_event perf_mem_events_amd[PERF_MEM_EVENTS__MAX] = {
	E(NULL,		NULL,		NULL,	false,	0),
	E(NULL,		NULL,		NULL,	false,	0),
	E_INIT("mem-ldst",	"%s//",		NULL,	false,	0, "%s/swfilt=1/"),
};

struct perf_mem_event perf_mem_events_amd_ldlat[PERF_MEM_EVENTS__MAX] = {
	E(NULL,		NULL,		NULL,	false,	0),
	E(NULL,		NULL,		NULL,	false,	0),
	E_INIT("mem-ldst",	"%s/ldlat=%u/",	NULL,	true,	0, "%s/ldlat=%u,swfilt=1/"),
};
