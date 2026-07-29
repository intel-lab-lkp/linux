// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <string.h>
#include <linux/string.h>
#include "util/evsel.h"
#include "util/record.h"
#include "util/evlist.h"
#include "util/debug.h"
#include <internal/xyarray.h>

void arch_evsel__set_sample_weight(struct evsel *evsel)
{
	evsel__set_sample_bit(evsel, WEIGHT_STRUCT);
}

/*
 * Check if HTM events have more data to collect.
 *
 * This function reads the HTM event counts. When the kernel driver
 * has more data available, it returns a non-zero count. When all
 * data has been collected, it returns zero.
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
		struct xyarray *xy = evsel->core.sample_id;

		if (strcmp(evsel__pmu_name(evsel), "htm"))
			continue;

		if (xy == NULL || evsel->core.fd == NULL)
			continue;

		if (xyarray__max_x(evsel->core.fd) != xyarray__max_x(xy) ||
		    xyarray__max_y(evsel->core.fd) != xyarray__max_y(xy)) {
			pr_debug("Unmatched FD vs sample ID array for HTM event\n");
			continue;
		}

		for (x = 0; x < xyarray__max_x(xy); x++) {
			for (y = 0; y < xyarray__max_y(xy); y++) {
				struct perf_counts_values count = { .val = 0 };

				if (perf_evsel__read(&evsel->core, x, y, &count) == 0)
					total_pending_records += count.val;
			}
		}
	}

	/* Collection is complete only when ALL hardware queues have no pending records */
	return (total_pending_records > 0) ? 1 : 0;
}
