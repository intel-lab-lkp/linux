#ifndef __PERF_FTRACE_H__
#define __PERF_FTRACE_H__

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <linux/list.h>

#include "target.h"

struct evlist;
struct hashamp;
struct stats;

struct perf_ftrace {
	struct evlist		*evlist;
	struct target		target;
	const char		*tracer;
	struct list_head	filters;
	struct list_head	notrace;
	struct list_head	graph_funcs;
	struct list_head	nograph_funcs;
	struct list_head	event_pair;
	struct hashmap		*profile_hash;
	unsigned long		percpu_buffer_size;
	bool			inherit;
	bool			use_nsec;
	unsigned int		bucket_range;
	unsigned int		min_latency;
	unsigned int		max_latency;
	unsigned int		bucket_num;
	bool			hide_empty;
	int			graph_depth;
	int			func_stack_trace;
	int			func_irq_info;
	int			graph_args;
	int			graph_retval;
	int			graph_retval_hex;
	int			graph_retaddr;
	int			graph_nosleep_time;
	int			graph_noirqs;
	int			graph_verbose;
	int			graph_thresh;
	int			graph_tail;
};

struct filter_entry {
	struct list_head	list;
	char			name[];
};

#define NUM_BUCKET  22  /* 20 + 2 (for outliers in both direction) */

#ifdef HAVE_BPF_SKEL

int perf_ftrace__latency_prepare_bpf(struct perf_ftrace *ftrace);
int perf_ftrace__latency_start_bpf(struct perf_ftrace *ftrace);
int perf_ftrace__latency_stop_bpf(struct perf_ftrace *ftrace);
int perf_ftrace__latency_read_bpf(struct perf_ftrace *ftrace,
				  int buckets[], struct stats *stats);
int perf_ftrace__latency_cleanup_bpf(struct perf_ftrace *ftrace);

#else  /* !HAVE_BPF_SKEL */

static inline int
perf_ftrace__latency_prepare_bpf(struct perf_ftrace *ftrace __maybe_unused)
{
	return -1;
}

static inline int
perf_ftrace__latency_start_bpf(struct perf_ftrace *ftrace __maybe_unused)
{
	return -1;
}

static inline int
perf_ftrace__latency_stop_bpf(struct perf_ftrace *ftrace __maybe_unused)
{
	return -1;
}

static inline int
perf_ftrace__latency_read_bpf(struct perf_ftrace *ftrace __maybe_unused,
			      int buckets[] __maybe_unused,
			      struct stats *stats __maybe_unused)
{
	return -1;
}

static inline int
perf_ftrace__latency_cleanup_bpf(struct perf_ftrace *ftrace __maybe_unused)
{
	return -1;
}

#endif  /* HAVE_BPF_SKEL */

#define FTRACE_MAX_CPUS 4096
#define FTRACE_MAX_STACK 256
#define FTRACE_PID_HASH_BITS 10
#define FTRACE_PID_HASH_SIZE (1 << FTRACE_PID_HASH_BITS)

#define FTRACE_IDLE_PID(cpu)       ((pid_t)(-1 - (cpu)))
#define FTRACE_UNKNOWN_PID(cpu)    ((pid_t)(-1 - FTRACE_MAX_CPUS - (cpu)))

struct ftrace_stack_frame {
	char *name;
	bool inlined;
	int indent;
	int raw_indent;
};

struct ftrace_task_state {
	struct hlist_node	node;
	pid_t			pid;
	struct ftrace_stack_frame *stack;
	int			max_stack;
	int			depth;
	int			inlined_depth;
	int			overflow_depth;
};

bool ftrace_parse_retaddr(const char *str, char *sym_name, size_t sym_len, u64 *offset);
void ftrace_filter_retaddr(char *str);
void ftrace_cpu_states__clear(void);
int ftrace_process_fgraph_line(struct perf_ftrace *ftrace, const char *line, FILE *out);

#endif  /* __PERF_FTRACE_H__ */
