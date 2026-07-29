// SPDX-License-Identifier: GPL-2.0
/*
 * HTM AUX tracing support
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/zalloc.h>
#include <stdlib.h>
#include <limits.h>
#include "../../util/evsel.h"
#include "../../util/evlist.h"
#include "../../util/session.h"
#include "../../util/debug.h"
#include "../../util/auxtrace.h"
#include "../../util/powerpc-htm.h"
#include "../../util/record.h"
#include <internal/lib.h> /* page_size */
#include <errno.h>

#define KiB(x) ((x) * 1024)

struct htm_recording {
	struct auxtrace_record	itr;
	struct evsel		*evsel;
};

static int
htm_recording_options(struct auxtrace_record *itr __maybe_unused,
		      struct evlist *evlist,
		      struct record_opts *opts)
{
	struct evsel *pos;

	opts->full_auxtrace = true;

	if (!opts->auxtrace_mmap_pages) {
		opts->auxtrace_mmap_pages = KiB(128) / page_size;
		if (opts->mmap_pages == UINT_MAX)
			opts->mmap_pages = KiB(256) / page_size;
	}

	evlist__for_each_entry(evlist, pos) {
		if (strcmp(evsel__pmu_name(pos), "htm"))
			continue;
		pos->core.attr.aux_watermark =
			opts->auxtrace_mmap_pages * (size_t)page_size / 2;
		pos->core.attr.sample_type |= PERF_SAMPLE_RAW;
		pos->core.attr.freq = 0;
		pos->core.attr.sample_period = 1;
		pos->needs_auxtrace_mmap = true;
	}

	return 0;
}

/* Count htm evsels in the evlist */
static int htm_nr_events(struct evlist *evlist)
{
	struct evsel *pos;
	int n = 0;

	evlist__for_each_entry(evlist, pos) {
		if (!strcmp(evsel__pmu_name(pos), "htm"))
			n++;
	}
	return n;
}

static size_t htm_info_priv_size(struct auxtrace_record *itr __maybe_unused,
				 struct evlist *evlist)
{
	return HTM_AUXTRACE_PRIV_SIZE(htm_nr_events(evlist));
}

/*
 * Fill the PERF_RECORD_AUXTRACE_INFO private data with:
 *   priv[POWERPC_HTM_PMU_TYPE]   = pmu->type of the first htm evsel
 *   priv[POWERPC_HTM_NUM_EVENTS] = number of htm evsels
 *   priv[POWERPC_HTM_EVENT_DATA + n*2]     = CPU for nth htm evsel
 *   priv[POWERPC_HTM_EVENT_DATA + n*2 + 1] = attr.config for nth htm evsel
 *
 * The CPU is the first CPU in the evsel's cpu map; for events opened with
 * cpu=N there is exactly one CPU.  The decode side uses event->auxtrace.cpu
 * to look up the matching config and derive (node, chip, core) for the
 * output file name.
 */
static int
htm_info_fill(struct auxtrace_record *itr,
	      struct perf_session *session,
	      struct perf_record_auxtrace_info *auxtrace_info,
	      size_t priv_size)
{
	struct htm_recording *htm_r = container_of(itr, struct htm_recording, itr);
	struct evlist *evlist = session->evlist;
	struct evsel *pos;
	int n = 0;
	int expected_n = htm_nr_events(evlist);

	if (priv_size != HTM_AUXTRACE_PRIV_SIZE(expected_n))
		return -EINVAL;

	auxtrace_info->type = PERF_AUXTRACE_POWERPC_HTM;
	auxtrace_info->priv[POWERPC_HTM_PMU_TYPE] = htm_r->evsel->core.attr.type;
	auxtrace_info->priv[POWERPC_HTM_NUM_EVENTS] = expected_n;

	evlist__for_each_entry(evlist, pos) {
		struct perf_cpu_map *cpus;
		int cpu;

		if (strcmp(evsel__pmu_name(pos), "htm"))
			continue;

		/*
		 * Get the CPU this evsel is pinned to.  For events opened
		 * with cpu=N, evsel__cpus() returns a single-entry map {N}
		 * at record time (not during replay).
		 */
		cpus = evsel__cpus(pos);
		if (cpus && perf_cpu_map__nr(cpus) > 0)
			cpu = perf_cpu_map__cpu(cpus, 0).cpu;
		else
			cpu = -1;

		auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + n * 2]     = cpu;
		auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + n * 2 + 1] =
							pos->core.attr.config;
		n++;
	}

	return 0;
}

static u64 htm_reference(struct auxtrace_record *itr __maybe_unused)
{
	return 0;
}

static void htm_free(struct auxtrace_record *itr)
{
	struct htm_recording *htm_r = container_of(itr, struct htm_recording, itr);

	free(htm_r);
}

struct auxtrace_record *htm_recording_init(struct evsel *pos, int *err)
{
	struct htm_recording *htm_r;

	/*
	 * To obtain the auxtrace buffer file descriptor, the auxtrace event
	 * must come first.
	 */
	evlist__to_front(pos->evlist, pos);

	htm_r = zalloc(sizeof(*htm_r));
	if (!htm_r) {
		pr_debug("htm_recording allocation failed (-ENOMEM)\n");
		*err = -ENOMEM;
		return NULL;
	}

	htm_r->evsel = pos;
	htm_r->itr.recording_options = htm_recording_options;
	htm_r->itr.info_priv_size    = htm_info_priv_size;
	htm_r->itr.info_fill         = htm_info_fill;
	htm_r->itr.free              = htm_free;
	htm_r->itr.reference         = htm_reference;
	return &htm_r->itr;
}
