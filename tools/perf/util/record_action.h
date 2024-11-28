/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_UTIL_RECORD_ACTION_H_
#define __PERF_UTIL_RECORD_ACTION_H_

#include <errno.h>
#include "evlist.h"

#ifdef HAVE_BPF_SKEL

int bpf_perf_record_init(void);

int bpf_perf_record(struct evlist *evlist, int argc, const char **argv);

#else /* !HAVE_BPF_SKEL */

static inline int bpf_perf_record(struct evlist *evlist __maybe_unused,
				  int argc __maybe_unused,
				  const char **argv __maybe_unused)
{
	return -EOPNOTSUPP;
}

static inline int bpf_perf_record_init(void)
{
	return 0;
}

#endif /* !HAVE_BPF_SKEL */

#endif /* __PERF_UTIL_RECORD_ACTION_H_ */
