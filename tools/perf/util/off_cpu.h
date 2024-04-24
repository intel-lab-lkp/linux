#ifndef PERF_UTIL_OFF_CPU_H
#define PERF_UTIL_OFF_CPU_H

#include <linux/perf_event.h>

struct evlist;
struct target;
struct perf_session;
struct record_opts;

#define OFFCPU_EVENT  "offcpu-time"

#define OFFCPU_SAMPLE_TYPES  (PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_IP | \
			      PERF_SAMPLE_TID | PERF_SAMPLE_TIME | \
			      PERF_SAMPLE_ID | PERF_SAMPLE_CPU | \
			      PERF_SAMPLE_PERIOD | PERF_SAMPLE_CALLCHAIN | \
			      PERF_SAMPLE_CGROUP)


#ifdef HAVE_BPF_SKEL
int off_cpu_prepare(struct evlist *evlist, struct target *target,
		    struct record_opts *opts);
ssize_t off_cpu_strip(struct evlist *evlist, struct mmap *mp,
		      char *dst, size_t size);
int off_cpu_prepare_parse(struct evlist *evlist);
#else
static inline int off_cpu_prepare(struct evlist *evlist __maybe_unused,
				  struct target *target __maybe_unused,
				  struct record_opts *opts __maybe_unused)
{
	return -1;
}
static inline ssize_t off_cpu_strip(struct evlist *evlist __maybe_unused,
				    struct mmap *mp __maybe_unused,
				    char *dst __maybe_unused,
				    size_t size __maybe_unused)
{
	return -1;
}
static inline int off_cpu_prepare_parse(struct evlist *evlist __maybe_unused)
{
	return -1;
}
#endif

#endif  /* PERF_UTIL_OFF_CPU_H */
