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
		size_t wm;

		if (strcmp(evsel__pmu_name(pos), "htm"))
			continue;
		wm = opts->auxtrace_mmap_pages * (size_t)page_size / 2;
		pos->core.attr.aux_watermark = min_t(size_t, wm, UINT_MAX);
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
 *   priv[POWERPC_HTM_NUM_EVENTS] = total number of HTM CPU entries
 *   priv[POWERPC_HTM_EVENT_DATA + n*2]     = CPU number for nth entry
 *   priv[POWERPC_HTM_EVENT_DATA + n*2 + 1] = attr.config for nth entry
 *
 * One entry is written per CPU in each evsel's cpu map.  An evsel opened
 * with -C 0,1,2 contributes three entries (one per CPU), each carrying
 * the same attr.config.  The decode side uses event->auxtrace.cpu to look
 * up the matching config and derive (node, chip, core) for the output
 * file name.
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
		int i, nr;

		if (strcmp(evsel__pmu_name(pos), "htm"))
			continue;

		/*
		 * Emit one (cpu, config) entry for every CPU in this evsel's
		 * map.  perf record -C 0,1,2 creates one evsel with a
		 * three-entry cpu map; each CPU gets its own AUX buffer and
		 * must be individually mapped so the decoder can match
		 * event->auxtrace.cpu to the correct (node, chip, core).
		 */
		cpus = evsel__cpus(pos);
		nr = cpus ? perf_cpu_map__nr(cpus) : 0;

		if (nr > 0) {
			for (i = 0; i < nr; i++) {
				int cpu = perf_cpu_map__cpu(cpus, i).cpu;

				auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + n * 2]     = cpu;
				auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + n * 2 + 1] =
								pos->core.attr.config;
				n++;
			}
		} else {
			/* cpu-agnostic evsel: record cpu = -1 */
			auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + n * 2]     = (u64)-1;
			auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + n * 2 + 1] =
							pos->core.attr.config;
			n++;
		}
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
