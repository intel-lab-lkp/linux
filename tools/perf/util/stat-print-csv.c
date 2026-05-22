/* SPDX-License-Identifier: GPL-2.0 */
#include "stat-print.h"
#include <linux/compiler.h>

int perf_stat__print_csv(struct evlist *evlist __maybe_unused,
			 const struct perf_stat_config *config __maybe_unused,
			 const struct target *target __maybe_unused,
			 const struct timespec *ts __maybe_unused,
			 int argc __maybe_unused,
			 const char **argv __maybe_unused)
{
	return 0;
}
