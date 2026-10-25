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

	ts->stack = calloc(FTRACE_MAX_STACK, sizeof(*ts->stack));
	if (!ts->stack) {
		free(ts);
		return NULL;
	}
	ts->max_stack = FTRACE_MAX_STACK;

	ts->pid = pid;
	ts->min_raw_indent = -1;
	key = hash_32((u32)pid, FTRACE_PID_HASH_BITS);
	head = &ftrace_task_hash[key];
	hlist_add_head(&ts->node, head);
	return ts;
}

/**
 * ftrace_task_state__grow_stack - Double the capacity of a task's call stack
 * @cs: Task state whose stack should be resized
 *
 * Reallocate @cs->stack with twice the current capacity, zero-initialising
 * the newly allocated frames.
 *
 * Return: 0 on success, or -ENOMEM if memory allocation fails.
 */
static int ftrace_task_state__grow_stack(struct ftrace_task_state *cs)
{
	int new_max = cs->max_stack ? cs->max_stack * 2 : FTRACE_MAX_STACK;
	struct ftrace_stack_frame *new_stack;

	new_stack = realloc(cs->stack, new_max * sizeof(*new_stack));
	if (!new_stack) {
		pr_err("Not enough memory to resize ftrace stack\n");
		return -ENOMEM;
	}

	memset(new_stack + cs->max_stack, 0, (new_max - cs->max_stack) * sizeof(*new_stack));
	cs->stack = new_stack;
	cs->max_stack = new_max;
	return 0;
}

/**
 * ftrace_task_state__rekey - Reassign a task state to a new PID
 * @ts: The task state to re-key
 * @new_pid: The new process ID to associate with the state
 *
 * Rehash @ts under @new_pid. If a task state for @new_pid already exists,
 * merge or replace it and update active CPU references accordingly.
 *
 * Return: A pointer to the active task state associated with @new_pid.
 */
static struct ftrace_task_state *ftrace_task_state__rekey(struct ftrace_task_state *ts,
							  pid_t new_pid)
{
	struct ftrace_task_state *existing;
	u32 key;
	int j, c;
	int to_copy;

	if (ts->pid == new_pid)
		return ts;

	hlist_del_init(&ts->node);

	existing = find_task_state(new_pid);
	if (existing) {
		int split_idx = existing->depth;
		int old_split_idx, post_count;
		int base_raw_indent, post_raw_indent;

		for (c = 0; c < FTRACE_MAX_CPUS; c++) {
			if (ftrace_cpu_tasks[c] == ts)
				ftrace_cpu_tasks[c] = existing;
		}

		/*
		 * Find the split point in existing where frames postdate ts.
		 * Frames 0 .. split_idx - 1 were pushed prior to ts.
		 * Frames split_idx .. existing->depth - 1 were pushed after ts.
		 */
		for (j = 0; j < existing->depth; j++) {
			if (existing->stack[j].seq > ts->seq) {
				split_idx = j;
				break;
			}
		}
		old_split_idx = split_idx;
		post_count = existing->depth - old_split_idx;

		/*
		 * Prune pre-ts frames on existing that exited before or during ts
		 * (i.e. whose raw_indent >= base_raw_indent, along with any
		 * synthesized inlined frames).
		 */
		base_raw_indent = -1;
		if (ts->depth > 0)
			base_raw_indent = ts->stack[0].raw_indent;
		if (ts->min_raw_indent >= 0 &&
		    (base_raw_indent < 0 || ts->min_raw_indent < base_raw_indent))
			base_raw_indent = ts->min_raw_indent;

		if (base_raw_indent >= 0) {
			while (split_idx > 0) {
				struct ftrace_stack_frame *top =
					&existing->stack[split_idx - 1];

				if (top->inlined) {
					free(top->name);
					top->name = NULL;
					if (existing->inlined_depth > 0)
						existing->inlined_depth--;
					split_idx--;
					existing->depth--;
				} else if (top->raw_indent >= base_raw_indent) {
					free(top->name);
					top->name = NULL;
					split_idx--;
					existing->depth--;
				} else {
					break;
				}
			}
		}

		/*
		 * If newer frames or exits exist on existing, prune any frames
		 * on ts that exited before them.
		 */
		post_raw_indent = -1;
		if (existing->seq > ts->seq) {
			if (post_count > 0)
				post_raw_indent = existing->stack[old_split_idx].raw_indent;
			if (existing->min_raw_indent >= 0 &&
			    (post_raw_indent < 0 || existing->min_raw_indent < post_raw_indent))
				post_raw_indent = existing->min_raw_indent;
		}

		while (post_raw_indent >= 0 && ts->depth > 0) {
			struct ftrace_stack_frame *top =
				&ts->stack[ts->depth - 1];

			if (top->inlined) {
				free(top->name);
				top->name = NULL;
				if (ts->inlined_depth > 0)
					ts->inlined_depth--;
				ts->depth--;
			} else if (top->raw_indent >= post_raw_indent) {
				free(top->name);
				top->name = NULL;
				ts->depth--;
			} else {
				break;
			}
		}

		to_copy = ts->depth;
		{
			int total_needed = split_idx + to_copy + post_count;

			while (total_needed > existing->max_stack) {
				if (ftrace_task_state__grow_stack(existing) < 0)
					break;
			}
			if (total_needed > existing->max_stack) {
				if (existing->max_stack > split_idx + post_count)
					to_copy = existing->max_stack - (split_idx + post_count);
				else
					to_copy = 0;
			}

			if (post_count > 0 && (split_idx + to_copy) != old_split_idx) {
				memmove(&existing->stack[split_idx + to_copy],
					&existing->stack[old_split_idx],
					post_count * sizeof(existing->stack[0]));
			}
			if (to_copy > 0) {
				memcpy(&existing->stack[split_idx], &ts->stack[0],
				       to_copy * sizeof(existing->stack[0]));

				for (j = 0; j < to_copy; j++) {
					if (ts->stack[j].inlined)
						existing->inlined_depth++;
				}
			}
			for (j = to_copy; j < ts->depth; j++)
				free(ts->stack[j].name);

			existing->depth = split_idx + to_copy + post_count;
		}

		if (ts->seq > existing->seq)
			existing->seq = ts->seq;
		if (existing->depth > 0) {
			existing->min_raw_indent = -1;
		} else {
			int m = existing->min_raw_indent;

			if (ts->min_raw_indent >= 0 &&
			    (m < 0 || ts->min_raw_indent < m))
				m = ts->min_raw_indent;
			existing->min_raw_indent = m;
		}

		free(ts->stack);
		free(ts);
		return existing;
	}

	ts->pid = new_pid;
	key = hash_32((u32)new_pid, FTRACE_PID_HASH_BITS);
	hlist_add_head(&ts->node, &ftrace_task_hash[key]);
	return ts;
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
 * function_graph trace record.
 *
 * Return: A pointer to the last character of the context columns (the final
 * '|' delimiter, or the CPU ')' when no delimiter follows it). Returns NULL
 * if @line is not a trace record.
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
	struct ftrace_task_state *cs;
	int cpu = -1;
	int leading_spaces;
	int top_raw_indent;
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

	cs = get_cpu_state(ftrace, cpu);
	if (!cs)
		return print_line(ftrace, line, out);

	cs->seq = ++ftrace_event_seq;

	after_pipe = pipe_char + 1;
	func_start = skip_spaces(after_pipe);
	leading_spaces = func_start - after_pipe;

	if (*func_start == '\0')
		return print_line(ftrace, line, out);

	is_exit = (*func_start == '}');

	if (is_exit) {
		char exit_name[128];

		if (cs->min_raw_indent < 0 || leading_spaces < cs->min_raw_indent)
			cs->min_raw_indent = leading_spaces;

		extract_exit_func_name(func_start, exit_name, sizeof(exit_name));

		top_raw_indent = -1;
		for (i = cs->depth - 1; i >= 0; i--) {
			top_raw_indent = cs->stack[i].raw_indent;
			break;
		}

		if (cs->overflow_depth > 0 &&
		    (top_raw_indent < 0 || leading_spaces > top_raw_indent)) {
			cs->overflow_depth--;
			return print_line(ftrace, line, out);
		}

		match_idx = -1;
		if (exit_name[0] != '\0') {
			for (i = cs->depth - 1; i >= 0; i--) {
				if (cs->stack[i].name &&
				    cs->stack[i].raw_indent == leading_spaces &&
				    strcmp(cs->stack[i].name, exit_name) == 0) {
					match_idx = i;
					break;
				}
			}
		}

		if (match_idx < 0 && exit_name[0] == '\0') {
			for (i = cs->depth - 1; i >= 0; i--) {
				if (cs->stack[i].raw_indent == leading_spaces) {
					match_idx = i;
					break;
				}
			}
		}

		if (match_idx >= 0) {
			cs->overflow_depth = 0;
			while (cs->depth > match_idx) {
				free(cs->stack[cs->depth - 1].name);
				cs->depth--;
			}
		} else if (cs->overflow_depth > 0) {
			cs->overflow_depth--;
		}

		return print_line(ftrace, line, out);
	}

	has_open_brace = (strchr(func_start, '{') != NULL);
	has_semicolon = (strchr(func_start, ';') != NULL);

	top_raw_indent = -1;
	for (i = cs->depth - 1; i >= 0; i--) {
		top_raw_indent = cs->stack[i].raw_indent;
		break;
	}

	if (cs->overflow_depth > 0 && top_raw_indent >= 0 &&
	    leading_spaces <= top_raw_indent)
		cs->overflow_depth = 0;

	if (has_open_brace && !has_semicolon) {
		char entry_name[128];

		extract_entry_func_name(func_start, entry_name, sizeof(entry_name));
		if (cs->depth >= cs->max_stack &&
		    ftrace_task_state__grow_stack(cs) < 0) {
			cs->overflow_depth++;
		} else {
			cs->stack[cs->depth].name = strdup(entry_name);
			if (!cs->stack[cs->depth].name) {
				pr_err("Not enough memory\n");
				return -ENOMEM;
			}
			cs->stack[cs->depth].inlined = false;
			cs->stack[cs->depth].indent = leading_spaces;
			cs->stack[cs->depth].raw_indent = leading_spaces;
			cs->stack[cs->depth].seq = cs->seq;
			cs->depth++;
			cs->min_raw_indent = -1;
		}
	}

	return print_line(ftrace, line, out);
}
