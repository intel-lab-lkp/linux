// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/string.h>
#include "util/evsel.h"
#include "util/record.h"
#include "util/evlist.h"
#include "util/debug.h"
#include <internal/xyarray.h>
#include <internal/lib.h>

void arch_evsel__set_sample_weight(struct evsel *evsel)
{
	evsel__set_sample_bit(evsel, WEIGHT_STRUCT);
}

/*
 * powerpc implementation of arch_perf_record__need_read().
 *
 * Reads event->count for every open HTM evsel by issuing a direct
 * read() on the event fd with a plain u64 buffer, bypassing the
 * PERF_FORMAT_GROUP path in perf_evsel__read().  When an HTM evsel is
 * a group sibling, evsel__config() sets PERF_FORMAT_GROUP on its attr;
 * perf_evsel__read() would then call perf_evsel__read_group() which
 * sizes the buffer by evsel->nr_members (0 for siblings), causing the
 * kernel to return -ENOSPC.  Reading the fd directly with sizeof(u64)
 * retrieves the HTM driver's plain pending-record count regardless of
 * group membership.
 *
 * Returns: 1 if more data exists, 0 if collection is complete
 */
int arch_perf_record__need_read(struct evlist *evlist)
{
	struct evsel *evsel;
	u64 total_pending_records = 0;
	int x, y;

	/* there was an error during record__open */
	if (!evlist)
		return 0;

	/* Read HTM event counts to check if more data is available */
	evlist__for_each_entry(evlist, evsel) {
		struct perf_evsel *rd_evsel;
		struct xyarray *xy;

		if (strcmp(evsel__pmu_name(evsel), "htm"))
			continue;

		/*
		 * For group siblings nr_members == 0, which makes
		 * perf_evsel__read_size() return 0 and readn() fail.
		 * Read through the leader instead; perf_evsel__read_group()
		 * extracts the leader's own count from the group buffer.
		 */
		if (evsel->core.leader != &evsel->core)
			rd_evsel = evsel->core.leader;
		else
			rd_evsel = &evsel->core;

		xy = rd_evsel->sample_id;

		if (xy == NULL || rd_evsel->fd == NULL)
			continue;

		if (xyarray__max_x(rd_evsel->fd) != xyarray__max_x(xy) ||
		    xyarray__max_y(rd_evsel->fd) != xyarray__max_y(xy)) {
			pr_debug("Unmatched FD vs sample ID array for HTM event\n");
			continue;
		}

		for (x = 0; x < xyarray__max_x(xy); x++) {
			for (y = 0; y < xyarray__max_y(xy); y++) {
				struct perf_counts_values count = { .val = 0 };

				if (perf_evsel__read(rd_evsel, x, y, &count) == 0)
					total_pending_records += count.val;
			}
		}
	}

	/* Collection is complete only when ALL hardware queues have no pending records */
	return (total_pending_records > 0) ? 1 : 0;
}
