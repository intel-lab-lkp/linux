// SPDX-License-Identifier: GPL-2.0
/*
 * HTM support
 */

#include "../../../util/record.h"
#include "evlist.h"
#include "evsel.h"
#include "session.h"
#include "debug.h"
#include <internal/xyarray.h>

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
	int found_htm = 0;

	/* there was an error during record__open */
	if (!evlist)
		return 0;

	/* First, check if any HTM events exist */
	evlist__for_each_entry(evlist, evsel) {
		if (strstr(evsel->name, "htm") != NULL)
			found_htm = 1;
	}

	if (!found_htm)
		return 0;

	/* Read HTM event counts to check if more data is available */
	evlist__for_each_entry(evlist, evsel) {
		struct xyarray *xy = evsel->core.sample_id;

		if (strstr(evsel->name, "htm") == NULL)
			continue;

		if (xy == NULL || evsel->core.fd == NULL)
			continue;
		if (xyarray__max_x(evsel->core.fd) != xyarray__max_x(xy) ||
			xyarray__max_y(evsel->core.fd) != xyarray__max_y(xy)) {
			pr_debug("Unmatched FD vs. sample ID: skip reading LOST count\n");
			continue;
		}

		for (int x = 0; x < xyarray__max_x(xy); x++) {
			for (int y = 0; y < xyarray__max_y(xy); y++) {
				struct perf_counts_values count;

				if (!strcmp(evsel->name, "dummy:u"))
					continue;

				if (strstr(evsel->name, "htm")) {
					perf_evsel__read(&evsel->core, x, y, &count);
					y = xyarray__max_y(xy);
					x = xyarray__max_x(xy);
				}
				if (!count.val)
					return 0;
			}
		}
	}

	return 1;
}
