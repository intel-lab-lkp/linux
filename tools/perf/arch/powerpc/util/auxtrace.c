// SPDX-License-Identifier: GPL-2.0
/*
 * VPA support
 */
#include <errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/zalloc.h>

#include "../../util/evlist.h"
#include "../../util/debug.h"
#include "../../util/auxtrace.h"
#include "../../util/powerpc-vpadtl.h"
#include "../../util/powerpc-htm.h"
#include "../../util/record.h"

struct auxtrace_record *auxtrace_record__init(struct evlist *evlist,
						int *err)
{
	struct evsel *pos;
	struct evsel *vpa_dtl_evsel = NULL;
	struct evsel *htm_evsel = NULL;

	/*
	 * Set err value to zero here. Any fail later
	 * will set appropriate return code to err.
	 */
	*err = 0;

	evlist__for_each_entry(evlist, pos) {
		if (pos->name && strstarts(pos->name, "vpa_dtl")) {
			pos->needs_auxtrace_mmap = true;
			/* Remember the first matching VPA DTL event */
			if (!vpa_dtl_evsel)
				vpa_dtl_evsel = pos;
		} else if (!strcmp(evsel__pmu_name(pos), "htm")) {
			pos->needs_auxtrace_mmap = true;
			/* Remember the first matching HTM event */
			if (!htm_evsel)
				htm_evsel = pos;
		}
	}

	/*
	 * Only one auxtrace PMU can be initialised per session.  Reject
	 * concurrent VPA DTL and HTM events: HTM AUX buffers would be
	 * collected without a PERF_RECORD_AUXTRACE_INFO record, making
	 * the trace undecodable.
	 */
	if (vpa_dtl_evsel && htm_evsel) {
		pr_err("Cannot record VPA DTL and HTM auxtrace events simultaneously\n");
		*err = -EINVAL;
		return NULL;
	}

	if (vpa_dtl_evsel)
		return vpa_dtl_recording_init(vpa_dtl_evsel, err);
	else if (htm_evsel)
		return htm_recording_init(htm_evsel, err);

	return NULL;
}
