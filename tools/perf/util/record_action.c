// SPDX-License-Identifier: GPL-2.0
/**
 * Read event sample data and execute the specified actions.
 */

#include "util/debug.h"
#include "util/parse-action.h"
#include "util/record_action.h"

int bpf_perf_record(struct evlist *evlist __maybe_unused,
		    int argc __maybe_unused, const char **argv __maybe_unused)
{
	event_actions__free();
	return 0;
}
