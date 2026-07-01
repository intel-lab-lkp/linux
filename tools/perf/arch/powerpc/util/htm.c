// SPDX-License-Identifier: GPL-2.0
/*
 * HTM support
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/string.h>
#include <time.h>

#include "../../util/cpumap.h"
#include "../../util/evsel.h"
#include "../../util/evlist.h"
#include "../../util/session.h"
#include "../../util/util.h"
#include "../../util/pmu.h"
#include "../../util/debug.h"
#include "../../util/auxtrace.h"
#include "../../util/powerpc-htm.h"
#include "../../util/record.h"
#include <internal/lib.h> // page_size
#include <errno.h>

#define KiB(x) ((x) * 1024)

static int
htm_recording_options(struct auxtrace_record *ar __maybe_unused,
			struct evlist *evlist __maybe_unused,
			struct record_opts *opts)
{
	struct evsel *pos;

	opts->full_auxtrace = true;

	if (opts->target.system_wide) {
		pr_info("System wide monitoring not supported, specify -C <cpu>\n");
		return -EINVAL;
	} else if (!opts->target.cpu_list) {
		pr_info("-C option not provided, specify -C <cpu> to use HTM event\n");
		return -EINVAL;
	}

	/*
	 * Set auxtrace_mmap_pages to minimum
	 * two pages
	 */
	if (!opts->auxtrace_mmap_pages) {
		opts->auxtrace_mmap_pages = KiB(128) / page_size;
		if (opts->mmap_pages == UINT_MAX)
			opts->mmap_pages = KiB(256) / page_size;
	}

	evlist__for_each_entry(evlist, pos) {
		if (strstarts(pos->name, "htm")) {
			pos->needs_auxtrace_mmap = true;
			pos->core.attr.aux_watermark = opts->auxtrace_mmap_pages * (size_t)page_size;
			break;
		}
	}

	return 0;
}

static size_t htm_info_priv_size(struct auxtrace_record *itr __maybe_unused,
					struct evlist *evlist __maybe_unused)
{
	return HTM_AUXTRACE_PRIV_SIZE;
}

static int
htm_info_fill(struct auxtrace_record *itr __maybe_unused,
		struct perf_session *session __maybe_unused,
		struct perf_record_auxtrace_info *auxtrace_info __maybe_unused,
		size_t priv_size __maybe_unused)
{
	return 0;
}

static u64 htm_reference(struct auxtrace_record *itr __maybe_unused)
{
	return 0;
}

static void htm_free(struct auxtrace_record *itr)
{
	free(itr);
}

struct auxtrace_record *htm_recording_init(struct evsel *pos)
{
	struct auxtrace_record *aux;

	/*
	 * To obtain the auxtrace buffer file descriptor, the auxtrace event
	 * must come first.
	 */
	evlist__to_front(pos->evlist, pos);

	aux = zalloc(sizeof(*aux));
	if (aux == NULL) {
		pr_debug("aux record is NULL\n");
		return NULL;
	}

	aux->recording_options = htm_recording_options;
	aux->info_priv_size = htm_info_priv_size;
	aux->info_fill = htm_info_fill;
	aux->free = htm_free;
	aux->reference = htm_reference;
	return aux;
}
