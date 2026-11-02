// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/zalloc.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include "debug.h"
#include "evlist.h"
#include "util/ftrace.h"
#include "util/string2.h"
#include <perf/threadmap.h>

#define FTRACE_MAX_CPUS 4096
#define FTRACE_MAX_STACK 256
#define FTRACE_PID_HASH_BITS 10
#define FTRACE_PID_HASH_SIZE (1 << FTRACE_PID_HASH_BITS)

#define FTRACE_IDLE_PID(cpu)       ((pid_t)(-1 - (cpu)))
#define FTRACE_UNKNOWN_PID(cpu)    ((pid_t)(-1 - FTRACE_MAX_CPUS - (cpu)))

/*
 * Represents a single call frame on a task's function_graph stack.
 *
 * @name:    Function name extracted from the trace entry.
 * @inlined: Whether the frame represents a synthesised inlined function.
 * @indent:  Indentation (in spaces) used to correlate entry and exit frames
 *           (or visual indentation for synthesised inlined frames).
 * @seq:     Monotonic event sequence number when the frame was entered.
 */
struct ftrace_stack_frame {
	char *name;
	bool inlined;
	int indent;
	u64 seq;
};

/*
 * Per-task call stack state for function_graph trace reconstruction.
 *
 * @node:           Hash list node indexed by PID.
 * @pid:            Task PID (or synthetic per-CPU ID when PID is unavailable).
 * @stack:          Dynamically resized array of active stack frames.
 * @max_stack:      Current allocated capacity (in frames) of @stack.
 * @depth:          Current number of active frames stored in @stack.
 * @overflow_depth: Nesting count of frames entered beyond @stack capacity,
 *                  preventing their exits from popping valid parent frames.
 * @seq:            Event sequence number of the most recent trace record.
 * @min_raw_indent: Minimum raw indentation observed on this task state, or -1 if none.
 * @min_indent_seq: Event sequence number of the shallowest observed trace record.
 */
struct ftrace_task_state {
	struct hlist_node node;
	pid_t pid;
	struct ftrace_stack_frame *stack;
	int max_stack;
	int depth;
	int overflow_depth;
	u64 seq;
	int min_raw_indent;
	u64 min_indent_seq;
};

static struct hlist_head ftrace_task_hash[FTRACE_PID_HASH_SIZE];
static struct ftrace_task_state *ftrace_cpu_tasks[FTRACE_MAX_CPUS];
static u64 ftrace_event_seq;

/**
 * ftrace_cpu_states__clear - Free allocated CPU and task stack states
 *
 * Free all memory associated with the per-task stack tracking states,
 * including the strings allocated for stack frames.
 */
void ftrace_cpu_states__clear(void)
{
	struct ftrace_task_state *ts;
	struct hlist_node *tmp;
	int i, j;

	for (i = 0; i < FTRACE_PID_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(ts, tmp, &ftrace_task_hash[i], node) {
			for (j = 0; j < ts->depth; j++)
				free(ts->stack[j].name);
			free(ts->stack);
			hlist_del_init(&ts->node);
			free(ts);
		}
	}

	memset(ftrace_cpu_tasks, 0, sizeof(ftrace_cpu_tasks));
	ftrace_event_seq = 0;
}

/**
 * find_task_state - Look up call stack state for a task by PID
 * @pid: Process ID of the task
 *
 * Return: A pointer to the task's stack state, or NULL if not found.
 */
static struct ftrace_task_state *find_task_state(pid_t pid)
{
	struct ftrace_task_state *ts;
	struct hlist_head *head;
	u32 key = hash_32((u32)pid, FTRACE_PID_HASH_BITS);

	head = &ftrace_task_hash[key];
	hlist_for_each_entry(ts, head, node) {
		if (ts->pid == pid)
			return ts;
	}

	return NULL;
}

/**
 * ftrace_task_state__grow_stack - Double the capacity of a task's call stack
 * @ts: Task state whose stack should be resized
 *
 * Reallocate @ts->stack with twice the current capacity, zero-initialising
 * the newly allocated frames.
 *
 * Return: 0 on success, or -ENOMEM if memory allocation fails.
 */
static int ftrace_task_state__grow_stack(struct ftrace_task_state *ts)
{
	int new_max = ts->max_stack ? ts->max_stack * 2 : FTRACE_MAX_STACK;
	struct ftrace_stack_frame *new_stack;

	new_stack = realloc(ts->stack, new_max * sizeof(*new_stack));
	if (!new_stack) {
		pr_err("Not enough memory to resize ftrace stack\n");
		return -ENOMEM;
	}

	memset(new_stack + ts->max_stack, 0, (new_max - ts->max_stack) * sizeof(*new_stack));
	ts->stack = new_stack;
	ts->max_stack = new_max;
	return 0;
}

/**
 * get_task_state - Get or allocate stack state for a given task PID
 * @pid: Process ID of the task
 *
 * Return: A pointer to the task's stack state, or NULL on allocation failure.
 */
static struct ftrace_task_state *get_task_state(pid_t pid)
{
	struct ftrace_task_state *ts = find_task_state(pid);
	struct hlist_head *head;
	u32 key;

	if (ts)
		return ts;

	ts = zalloc(sizeof(*ts));
	if (!ts)
		return NULL;

	if (ftrace_task_state__grow_stack(ts) < 0) {
		free(ts);
		return NULL;
	}
	ts->min_raw_indent = -1;

	ts->pid = pid;
	key = hash_32((u32)pid, FTRACE_PID_HASH_BITS);
	head = &ftrace_task_hash[key];
	hlist_add_head(&ts->node, head);
	return ts;
}

/**
 * ftrace_task_state__record_indent - Record an indentation watermark for a task
 * @ts: Task state to update
 * @raw_indent: Raw indentation (leading spaces) of the observed trace record
 *
 * Record the shallowest indentation observed on @ts along with its event
 * sequence number so that earlier frames that have already exited can be
 * pruned when merging task states across CPUs.
 */
static void ftrace_task_state__record_indent(struct ftrace_task_state *ts,
					     int raw_indent)
{
	if (ts->min_raw_indent < 0 || raw_indent <= ts->min_raw_indent) {
		ts->min_raw_indent = raw_indent;
		ts->min_indent_seq = ts->seq;
	}
}

/**
 * ftrace_task_state__prune_stack - Prune frames that exited on another task state
 * @ts: Task state whose call stack should be pruned
 * @other: Other task state representing the same task on another CPU
 *
 * Remove any frames on @ts->stack that are known to have exited based on
 * subsequent trace events recorded on @other (i.e. @other->min_indent_seq >
 * frame->seq and @other->min_raw_indent <= frame->indent). Synthesised
 * inline frames are retained if and only if their enclosing real frame
 * survives.
 */
static void ftrace_task_state__prune_stack(struct ftrace_task_state *ts,
					   const struct ftrace_task_state *other)
{
	bool keep_inlines = false;
	bool seen_real = false;
	int i, dst = 0;

	for (i = ts->depth - 1; i >= 0; i--) {
		struct ftrace_stack_frame *frame = &ts->stack[i];
		int min_indent = (other->min_indent_seq > frame->seq) ?
				 other->min_raw_indent : -1;
		bool prune;

		if (frame->inlined) {
			if (seen_real)
				prune = !keep_inlines;
			else
				prune = (min_indent >= 0);
		} else {
			prune = (min_indent >= 0 && frame->indent >= min_indent);
			seen_real = true;
			keep_inlines = !prune;
		}

		if (prune)
			zfree(&frame->name);
	}

	for (i = 0; i < ts->depth; i++) {
		if (ts->stack[i].name) {
			if (dst != i)
				ts->stack[dst] = ts->stack[i];
			dst++;
		}
	}
	ts->depth = dst;
}

/**
 * ftrace_task_state__rekey - Reassign a task state to a new PID
 * @ts: The task state to re-key
 * @new_pid: The new process ID to associate with the state
 *
 * Rehash @ts under @new_pid. If a task state for @new_pid already exists,
 * merge or replace it and return the unified task state.
 *
 * Return: A pointer to the active task state associated with @new_pid.
 */
static struct ftrace_task_state *ftrace_task_state__rekey(struct ftrace_task_state *ts,
							  pid_t new_pid)
{
	struct ftrace_task_state *existing;
	int orig_existing_depth;
	int total_needed;
	int to_copy;
	u32 key;
	int j;

	if (ts->pid == new_pid)
		return ts;

	hlist_del_init(&ts->node);

	existing = find_task_state(new_pid);
	if (!existing) {
		ts->pid = new_pid;
		key = hash_32((u32)new_pid, FTRACE_PID_HASH_BITS);
		hlist_add_head(&ts->node, &ftrace_task_hash[key]);
		return ts;
	}

	/*
	 * Prune frames on existing and ts that exited during subsequent
	 * execution recorded on the other state.
	 */
	ftrace_task_state__prune_stack(existing, ts);
	ftrace_task_state__prune_stack(ts, existing);

	orig_existing_depth = existing->depth;
	to_copy = ts->depth;
	total_needed = orig_existing_depth + to_copy;

	while (total_needed > existing->max_stack) {
		if (ftrace_task_state__grow_stack(existing) < 0)
			break;
	}
	if (total_needed > existing->max_stack) {
		if (existing->max_stack > orig_existing_depth)
			to_copy = existing->max_stack - orig_existing_depth;
		else
			to_copy = 0;
	}

	if (to_copy > 0) {
		int e_idx = 0, t_idx = 0, out_idx = 0;

		if (orig_existing_depth > 0) {
			memmove(&existing->stack[to_copy],
				&existing->stack[0],
				orig_existing_depth * sizeof(existing->stack[0]));
		}

		while (e_idx < orig_existing_depth && t_idx < to_copy) {
			if (existing->stack[to_copy + e_idx].seq <=
			    ts->stack[t_idx].seq) {
				existing->stack[out_idx++] =
					existing->stack[to_copy + e_idx++];
			} else {
				existing->stack[out_idx++] =
					ts->stack[t_idx++];
			}
		}
		while (t_idx < to_copy)
			existing->stack[out_idx++] = ts->stack[t_idx++];
		while (e_idx < orig_existing_depth) {
			existing->stack[out_idx++] =
				existing->stack[to_copy + e_idx++];
		}
	}

	for (j = to_copy; j < ts->depth; j++)
		free(ts->stack[j].name);

	existing->depth = orig_existing_depth + to_copy;

	if (ts->seq > existing->seq)
		existing->seq = ts->seq;

	if (ts->min_raw_indent >= 0 &&
	    (existing->min_raw_indent < 0 ||
	     ts->min_raw_indent < existing->min_raw_indent ||
	     (ts->min_raw_indent == existing->min_raw_indent &&
	      ts->min_indent_seq > existing->min_indent_seq))) {
		existing->min_raw_indent = ts->min_raw_indent;
		existing->min_indent_seq = ts->min_indent_seq;
	}

	free(ts->stack);
	free(ts);
	return existing;
}

/**
 * ftrace__get_single_pid - Retrieve the single target process ID being traced
 * @ftrace: Perf ftrace context
 *
 * Check whether @ftrace is configured to trace a single process without
 * child inheritance. If the thread map contains exactly one valid process
 * ID and child task inheritance is disabled, return that PID so that
 * initial per-CPU task states can be bound directly to the target process.
 *
 * Return: The target process ID if tracing a single process without
 *         inheritance, or -1 otherwise.
 */
static pid_t ftrace__get_single_pid(struct perf_ftrace *ftrace)
{
	struct perf_thread_map *threads;
	pid_t pid;

	if (!ftrace || !ftrace->evlist || ftrace->inherit)
		return -1;

	threads = evlist__core(ftrace->evlist)->threads;
	if (!threads || perf_thread_map__nr(threads) != 1)
		return -1;

	pid = perf_thread_map__pid(threads, 0);
	return (pid > 0) ? pid : -1;
}

/**
 * get_cpu_state - Get or allocate active task stack state for a given CPU
 * @ftrace: Perf ftrace context
 * @cpu: The CPU number
 *
 * Return: A pointer to the CPU's active task stack state, or NULL on error or
 * out-of-bounds CPU index.
 */
static struct ftrace_task_state *get_cpu_state(struct perf_ftrace *ftrace, int cpu)
{
	pid_t single_pid;

	if (cpu < 0 || cpu >= FTRACE_MAX_CPUS)
		return NULL;

	if (!ftrace_cpu_tasks[cpu]) {
		single_pid = ftrace__get_single_pid(ftrace);
		if (single_pid > 0)
			ftrace_cpu_tasks[cpu] = get_task_state(single_pid);
		else
			ftrace_cpu_tasks[cpu] = get_task_state(FTRACE_UNKNOWN_PID(cpu));
	}

	return ftrace_cpu_tasks[cpu];
}

/**
 * parse_context_switch - Parse context switch banner and update CPU's task
 * @line: The trace line containing the banner
 *
 * Check if @line is a context switch banner (e.g. " 0) <idle>-0 => migrati-19").
 * If so, extract the CPU number and both the outgoing and incoming task PIDs,
 * re-key any initial unknown task state on this CPU to the outgoing PID, and
 * update the active task mapping for that CPU to the incoming PID.
 *
 * Return: true if @line was a context switch banner, false otherwise.
 */
static bool parse_context_switch(const char *line)
{
	const char *arrow = strstr(line, " => ");
	const char *p;
	const char *next;
	const char *hyphen;
	const char *h;
	char *endptr;
	char *end_pid;
	long cpu_val;
	long prev_pid;
	long next_pid;
	pid_t real_prev;
	pid_t real_next;

	if (!arrow)
		return false;

	/* Find CPU number before ')' */
	p = skip_spaces(line);
	cpu_val = strtol(p, &endptr, 10);
	if (endptr == p || *endptr != ')')
		return false;

	if (cpu_val < 0 || cpu_val >= FTRACE_MAX_CPUS)
		return false;

	/*
	 * Parse outgoing task PID before " => " (" CPU) comm-pid => ...").
	 * Scan backwards from arrow to find the hyphen before the numeric PID.
	 */
	h = arrow - 1;
	while (h > endptr && *h == ' ')
		h--;
	while (h > endptr && *h != '-')
		h--;
	if (h <= endptr || *h != '-')
		return false;

	prev_pid = strtol(h + 1, &end_pid, 10);
	if (end_pid == h + 1)
		return false;

	/* Parse target task PID after " => " (" => comm-pid") */
	next = skip_spaces(arrow + 4);
	hyphen = strrchr(next, '-');
	if (!hyphen)
		return false;

	next_pid = strtol(hyphen + 1, &end_pid, 10);
	if (end_pid == hyphen + 1)
		return false;

	real_prev = (prev_pid == 0) ? FTRACE_IDLE_PID(cpu_val) : (pid_t)prev_pid;
	real_next = (next_pid == 0) ? FTRACE_IDLE_PID(cpu_val) : (pid_t)next_pid;

	/*
	 * If no context switch was seen previously on this CPU, its initial
	 * pre-banner trace records were accumulated under an unknown synthetic
	 * PID. Re-key that state to real_prev so the initial task's stack and
	 * frames are preserved when it schedules back in.
	 */
	if (ftrace_cpu_tasks[cpu_val] &&
	    ftrace_cpu_tasks[cpu_val]->pid == FTRACE_UNKNOWN_PID(cpu_val)) {
		ftrace_cpu_tasks[cpu_val] =
			ftrace_task_state__rekey(ftrace_cpu_tasks[cpu_val], real_prev);
	}

	ftrace_cpu_tasks[cpu_val] = get_task_state(real_next);
	return true;
}

/**
 * ftrace_parse_retaddr - Parse return address comment from trace line
 * @str: The trace line containing the comment
 * @sym_name: Buffer to store the parsed symbol name
 * @sym_len: Length of the @sym_name buffer
 * @offset: Pointer to store the parsed offset
 *
 * Extract the function name and offset from a funcgraph-retaddr comment
 * (e.g. "<-load_elf_phdrs+0x6c/0xb0").
 *
 * Return: true if successfully parsed, false otherwise.
 */
bool ftrace_parse_retaddr(const char *str, char *sym_name, size_t sym_len, u64 *offset)
{
	const char *p = strstr(str, "<-");
	const char *plus;
	const char *bracket;
	size_t name_len;

	if (!p)
		return false;

	p += 2;
	while (*p == ' ')
		p++;

	if (*p == '[') {
		bracket = strchr(p, ']');
		if (bracket)
			p = bracket + 1;
		while (*p == ' ')
			p++;
	}

	plus = strchr(p, '+');
	if (!plus)
		return false;

	name_len = plus - p;
	if (name_len == 0 || name_len >= sym_len)
		return false;

	memcpy(sym_name, p, name_len);
	sym_name[name_len] = '\0';

	*offset = strtoull(plus + 1, NULL, 16);
	return true;
}

/**
 * ftrace_filter_retaddr - Remove return address comment from trace line
 * @str: The trace line to modify
 *
 * Find and strip the funcgraph-retaddr comment from the trace line
 * in-place, removing trailing spaces where applicable.
 */
void ftrace_filter_retaddr(char *str)
{
	char *start = strstr(str, "/* <-");
	char *end;
	char *ret_ptr;

	if (!start)
		return;

	end = strstr(start, "*/");
	if (!end)
		return;

	ret_ptr = strstr(start, " ret=");
	if (ret_ptr && ret_ptr < end) {
		memmove(start + 3, ret_ptr + 1, strlen(ret_ptr + 1) + 1);
		return;
	}

	end += 2; /* skip end-of-comment delimiter */

	/* Also backtrack any spaces before comment */
	while (start > str && *(start - 1) == ' ')
		start--;

	memmove(start, end, strlen(end) + 1);
}

/**
 * extract_entry_func_name - Extract the function name from an entry record
 * @str: The trace line's function column string
 * @buf: Buffer to store the extracted name
 * @size: Size of the @buf
 *
 * Parse the function name out of an entry trace record by stripping away
 * parentheses or braces.
 */
static void extract_entry_func_name(const char *str, char *buf, size_t size)
{
	size_t i = 0;

	while (*str && *str != '(' && *str != '{' && !isspace(*str) && i < size - 1)
		buf[i++] = *str++;
	buf[i] = '\0';
}

/**
 * extract_exit_func_name - Extract the function name from an exit record
 * @str: The trace line's function column string
 * @buf: Buffer to store the extracted name
 * @size: Size of the @buf
 *
 * Parse the original function name out of the trailing comment in an
 * exit trace record.
 */
static void extract_exit_func_name(const char *str, char *buf, size_t size)
{
	const char *p = strchr(str, '}');
	size_t i = 0;

	buf[0] = '\0';
	if (!p)
		return;

	p = strstr(p, "/*");
	if (!p)
		return;

	p = skip_spaces(p + 2);
	while (*p && !isspace(*p) && *p != '*' && *p != '/' && i < size - 1)
		buf[i++] = *p++;
	buf[i] = '\0';
}

/**
 * find_func_column - Locate the function call column in a trace record
 * @ftrace: Perf ftrace context
 * @line: The trace record to parse
 * @cpu: Pointer to store the CPU number
 *
 * Locate the context columns that precede the function call column of a
 * function_graph trace record. Depending on the funcgraph-* options set
 * up by perf, a record looks like:
 *
 *   [ abstime |]  CPU)  [ comm-pid |]  [ duration |]  func
 *
 * The abstime and comm-pid columns are only present with --graph-opts
 * verbose.
 *
 * Return: A pointer to the last character of the context columns (the final
 * '|' delimiter). Returns NULL if @line is not a trace record.
 */
static const char *find_func_column(const struct perf_ftrace *ftrace,
				    const char *line, int *cpu)
{
	const char *p = line;
	char *endptr;
	long val;

	if (ftrace->graph_verbose) {
		p = strchr(p, '|');
		if (!p)
			return NULL;
		p++;
	}

	p = skip_spaces(p);
	val = strtol(p, &endptr, 10);
	if (endptr == p || *endptr != ')')
		return NULL;
	p = endptr;

	if (ftrace->graph_verbose) {
		const char *proc_start = p;

		p = strchr(p, '|');
		if (!p)
			return NULL;

		if (val >= 0 && val < FTRACE_MAX_CPUS) {
			const char *h = p - 1;

			while (h >= proc_start && *h != '-')
				h--;
			if (h >= proc_start && *h == '-') {
				long pid_val = strtol(h + 1, NULL, 10);
				pid_t task_pid = (pid_val == 0) ?
						 FTRACE_IDLE_PID(val) :
						 (pid_t)pid_val;

				ftrace_cpu_tasks[(int)val] = get_task_state(task_pid);
			}
		}

		/*
		 * In verbose mode, latency-format is also enabled by
		 * set_tracing_funcgraph_verbose(), emitting a "<lat-fmt> |"
		 * column after the comm-pid column.
		 */
		p = strchr(p + 1, '|');
		if (!p)
			return NULL;
	}

	p = strchr(p + 1, '|');
	if (!p)
		return NULL;

	*cpu = (int)val;
	return p;
}

/**
 * print_line - Output a trace line verbatim, filtering return addresses if needed
 * @ftrace: Perf ftrace context
 * @line: The trace line to output
 * @out: File stream to output to
 *
 * Output @line to @out. Unless ftrace->graph_retaddr is set, return address
 * comments are filtered out first.
 *
 * Return: 0 on success, or a negative error code (e.g. -ENOMEM) on failure.
 */
static int print_line(struct perf_ftrace *ftrace, const char *line, FILE *out)
{
	char *filtered;

	if (ftrace->graph_retaddr) {
		fprintf(out, "%s\n", line);
		return 0;
	}

	filtered = strdup(line);
	if (!filtered) {
		pr_err("Not enough memory\n");
		return -ENOMEM;
	}

	ftrace_filter_retaddr(filtered);
	fprintf(out, "%s\n", filtered);
	free(filtered);
	return 0;
}

/**
 * ftrace_process_fgraph_line - Process and output a function graph trace line
 * @ftrace: Perf ftrace context
 * @line: The trace line to process
 * @out: File stream to output to
 *
 * Parse function entries and exits, maintain the task's call stack, and
 * output the trace line.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int ftrace_process_fgraph_line(struct perf_ftrace *ftrace, const char *line, FILE *out)
{
	const char *pipe_char;
	const char *after_pipe;
	const char *func_start;
	struct ftrace_task_state *ts;
	int cpu = -1;
	int leading_spaces;
	int top_indent;
	int match_idx;
	int i;
	bool is_exit;
	bool has_open_brace;
	bool has_semicolon;

	pipe_char = find_func_column(ftrace, line, &cpu);
	if (!pipe_char) {
		parse_context_switch(line);
		return print_line(ftrace, line, out);
	}

	ts = get_cpu_state(ftrace, cpu);
	if (!ts)
		return print_line(ftrace, line, out);

	ts->seq = ++ftrace_event_seq;

	after_pipe = pipe_char + 1;
	func_start = skip_spaces(after_pipe);
	leading_spaces = func_start - after_pipe;

	if (*func_start == '\0')
		return print_line(ftrace, line, out);

	is_exit = (*func_start == '}');

	if (is_exit) {
		char exit_name[128];

		ftrace_task_state__record_indent(ts, leading_spaces);

		extract_exit_func_name(func_start, exit_name, sizeof(exit_name));

		top_indent = -1;
		if (ts->depth > 0)
			top_indent = ts->stack[ts->depth - 1].indent;

		if (ts->overflow_depth > 0 &&
		    (top_indent < 0 || leading_spaces > top_indent)) {
			ts->overflow_depth--;
			return print_line(ftrace, line, out);
		}

		match_idx = -1;
		if (exit_name[0] != '\0') {
			for (i = ts->depth - 1; i >= 0; i--) {
				if (ts->stack[i].name &&
				    ts->stack[i].indent == leading_spaces &&
				    strcmp(ts->stack[i].name, exit_name) == 0) {
					match_idx = i;
					break;
				}
			}
		}

		if (match_idx < 0 && exit_name[0] == '\0') {
			for (i = ts->depth - 1; i >= 0; i--) {
				if (ts->stack[i].indent == leading_spaces) {
					match_idx = i;
					break;
				}
			}
		}

		if (match_idx >= 0) {
			ts->overflow_depth = 0;
			while (ts->depth > match_idx) {
				free(ts->stack[ts->depth - 1].name);
				ts->depth--;
			}
		} else if (ts->overflow_depth > 0) {
			ts->overflow_depth--;
		}

		return print_line(ftrace, line, out);
	}

	has_open_brace = (strchr(func_start, '{') != NULL);
	has_semicolon = (strchr(func_start, ';') != NULL);

	if (has_open_brace || has_semicolon)
		ftrace_task_state__record_indent(ts, leading_spaces);

	top_indent = -1;
	if (ts->depth > 0)
		top_indent = ts->stack[ts->depth - 1].indent;

	if (ts->overflow_depth > 0 && top_indent >= 0 &&
	    leading_spaces <= top_indent)
		ts->overflow_depth = 0;

	if (has_open_brace && !has_semicolon) {
		char entry_name[128];

		while (ts->depth > 0 &&
		       ts->stack[ts->depth - 1].indent >= leading_spaces) {
			free(ts->stack[ts->depth - 1].name);
			ts->depth--;
		}

		extract_entry_func_name(func_start, entry_name, sizeof(entry_name));
		if (ts->depth >= ts->max_stack &&
		    ftrace_task_state__grow_stack(ts) < 0) {
			ts->overflow_depth++;
		} else {
			ts->stack[ts->depth].name = strdup(entry_name);
			if (!ts->stack[ts->depth].name) {
				pr_err("Not enough memory\n");
				return -ENOMEM;
			}
			ts->stack[ts->depth].inlined = false;
			ts->stack[ts->depth].indent = leading_spaces;
			ts->stack[ts->depth].seq = ts->seq;
			ts->depth++;
		}
	}

	return print_line(ftrace, line, out);
}
