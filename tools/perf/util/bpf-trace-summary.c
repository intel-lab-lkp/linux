/* SPDX-License-Identifier: GPL-2.0 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "syscalltbl.h"
#include "util/trace.h"
#include "util/util.h"
#include <bpf/bpf.h>
#include <linux/time64.h>

#include "bpf_skel/syscall_summary.h"
#include "bpf_skel/syscall_summary.skel.h"


static struct syscall_summary_bpf *skel;

int trace_prepare_bpf_summary(void)
{
	skel = syscall_summary_bpf__open_and_load();
	if (skel == NULL) {
		fprintf(stderr, "failed to load syscall summary bpf skeleton\n");
		return -1;
	}

	if (syscall_summary_bpf__attach(skel) < 0) {
		fprintf(stderr, "failed to attach syscall summary bpf skeleton\n");
		return -1;
	}

	return 0;
}

void trace_start_bpf_summary(void)
{
	skel->bss->enabled = 1;
}

void trace_end_bpf_summary(void)
{
	skel->bss->enabled = 0;
}

struct syscall_data {
	int syscall_nr;
	struct syscall_stats stats;
};

static int datacmp(const void *a, const void *b)
{
	const struct syscall_data *sa = a;
	const struct syscall_data *sb = b;

	return sa->stats.total_time > sb->stats.total_time ? -1 : 1;
}

int trace_print_bpf_summary(struct syscalltbl *sctbl, FILE *fp)
{
	struct syscall_key *prev_key, key;
	struct syscall_data *data = NULL;
	struct bpf_map *map = skel->maps.syscall_stats_map;
	int nr_data = 0;
	int printed = 0;

	/* get stats from the bpf map */
	prev_key = NULL;
	while (!bpf_map__get_next_key(map, prev_key, &key, sizeof(key))) {
		struct syscall_stats stat;

		if (!bpf_map__lookup_elem(map, &key, sizeof(key), &stat, sizeof(stat), 0)) {
			struct syscall_data *tmp, *pos;

			tmp = realloc(data, sizeof(*data) * (nr_data + 1));
			if (tmp == NULL)
				break;

			data = tmp;
			pos = &data[nr_data++];

			pos->syscall_nr = key.nr;
			memcpy(&pos->stats, &stat, sizeof(stat));
		}

		prev_key = &key;
	}

	qsort(data, nr_data, sizeof(*data), datacmp);

	printed += fprintf(fp, "\n");

	printed += fprintf(fp, "   syscall            calls  errors  total       min       avg       max       stddev\n");
	printed += fprintf(fp, "                                     (msec)    (msec)    (msec)    (msec)        (%%)\n");
	printed += fprintf(fp, "   --------------- --------  ------ -------- --------- --------- ---------     ------\n");

	for (int i = 0; i < nr_data; i++) {
		struct syscall_data *pos = &data[i];
		double total = (double)(pos->stats.total_time) / NSEC_PER_MSEC;
		double min = (double)(pos->stats.min_time) / NSEC_PER_MSEC;
		double max = (double)(pos->stats.max_time) / NSEC_PER_MSEC;
		double avg = total / pos->stats.count;

		printed += fprintf(fp, "   %-15s", syscalltbl__name(sctbl, pos->syscall_nr));
		printed += fprintf(fp, " %8u %6u %9.3f %9.3f %9.3f %9.3f %9.2f%%\n",
				   pos->stats.count, pos->stats.error, total, min, avg, max,
				   /*stddev=*/0.0);
	}

	printed += fprintf(fp, "\n\n");
	free(data);

	return printed;
}

void trace_cleanup_bpf_summary(void)
{
	syscall_summary_bpf__destroy(skel);
}
