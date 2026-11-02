// SPDX-License-Identifier: GPL-2.0
/*
 * ftrace.c - Ftrace utilities and inlined function processing
 *
 * Copyright (C) 2026 Aaron Tomlin <atomlin@atomlin.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/types.h>
#include <linux/kernel.h>
#include <linux/zalloc.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/string.h>
#include "debug.h"
#include "dso.h"
#include "evlist.h"
#include "machine.h"
#include "map.h"
#include "srcline.h"
#include "symbol.h"
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
 * @indent:  Indentation (in spaces): original leading spaces from the kernel
 *           trace stream after '|' for real frames, or visual indentation used
 *           when printing to output for synthesised inlined frames.
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
 * @node:                  Hash list node indexed by PID.
 * @pid:                   Task PID (or synthetic per-CPU ID when PID is unavailable).
 * @stack:                 Dynamically resized array of active stack frames.
 * @max_stack:             Current allocated capacity (in frames) of @stack.
 * @depth:                 Current number of active frames stored in @stack.
 * @seq:                   Event sequence number of the most recent trace record.
 * @min_raw_indent:        Minimum raw indentation observed on this task state.
 * @min_indent_seq:        Event sequence number of @min_raw_indent.
 * @in_target:             Whether the task is currently within a matched -G target tree.
 * @target_root_depth:     Stack depth at the entry of the matched target root function.
 * @target_root_frame_idx: Stack index of the matched target root frame.
 * @target_root_indent:    Raw indentation of the matched target root function.
 * @target_root_name:      Symbol name of the matched target root function.
 * @orphan_depth:          Nesting depth of suppressed detached/orphan subtrees.
 * @orphan_base_indent:    Raw indentation of the outermost active orphan frame.
 * @orphan_seq:            Sequence number of the most recent event in the orphan subtree.
 * @pending_irq_ent:       Buffered IRQ entry banner awaiting target caller verification.
 * @suppress_irq_ret:      Whether the matching IRQ return banner should be suppressed.
 */
struct ftrace_task_state {
	struct hlist_node node;
	pid_t pid;
	struct ftrace_stack_frame *stack;
	int max_stack;
	int depth;
	u64 seq;
	int min_raw_indent;
	u64 min_indent_seq;
	bool in_target;
	int target_root_depth;
	int target_root_frame_idx;
	int target_root_indent;
	char target_root_name[128];
	int orphan_depth;
	int orphan_base_indent;
	u64 orphan_seq;
	char pending_irq_ent[256];
	bool suppress_irq_ret;
};

static struct hlist_head ftrace_task_hash[FTRACE_PID_HASH_SIZE];
static struct ftrace_task_state *ftrace_cpu_tasks[FTRACE_MAX_CPUS];
static u64 ftrace_event_seq;

/**
 * ftrace_cpu_states__clear - Free allocated CPU and task stack states
 *
 * Free all memory associated with the per-task stack tracking states
 * used for inlined function resolution, including the strings
 * allocated for stack frames.
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

	ts->pid = pid;
	ts->min_raw_indent = -1;
	key = hash_32((u32)pid, FTRACE_PID_HASH_BITS);
	head = &ftrace_task_hash[key];
	hlist_add_head(&ts->node, head);
	return ts;
}

/**
 * reset_target_root_state - Clear graph target filter state for a task
 * @ts: Task state to reset
 *
 * Reset all state tracking associated with an active graph target filter,
 * restoring the task to an out-of-target state.
 */
static void reset_target_root_state(struct ftrace_task_state *ts)
{
	ts->in_target = false;
	ts->target_root_depth = 0;
	ts->target_root_frame_idx = 0;
	ts->target_root_indent = 0;
	ts->target_root_name[0] = '\0';
	ts->orphan_depth = 0;
	ts->orphan_base_indent = 0;
	ts->orphan_seq = 0;
	ts->pending_irq_ent[0] = '\0';
	ts->suppress_irq_ret = false;
}

/**
 * ftrace_task_state__inlined_depth - Count active synthesised inline frames on stack
 * @ts: Task state to inspect
 *
 * Return: The number of active synthesised inline frames currently on @ts->stack.
 */
static int ftrace_task_state__inlined_depth(const struct ftrace_task_state *ts)
{
	int count = 0;

	for (int i = 0; i < ts->depth; i++) {
		if (ts->stack[i].inlined)
			count++;
	}
	return count;
}

/**
 * ftrace_task_state__record_indent - Record an indentation watermark for a task
 * @ts: Task state to update
 * @raw_indent: Raw indentation (leading spaces) of the observed trace record
 *
 * Track the minimum raw indentation observed on @ts and the latest sequence
 * number at which it occurred.
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
 * the minimum-indent watermark recorded on @other (i.e. @other->min_indent_seq
 * > frame->seq and @other->min_raw_indent <= frame->indent). Synthesised
 * inline frames are retained if and only if their enclosing real frame survives.
 */
static void ftrace_task_state__prune_stack(struct ftrace_task_state *ts,
					   const struct ftrace_task_state *other)
{
	bool keep_inlines = false;
	bool seen_real = false;
	int i, dst = 0;

	if (ts->in_target && ts->target_root_frame_idx >= ts->depth)
		reset_target_root_state(ts);

	if (ts->orphan_depth > 0) {
		int min_indent = (other->min_indent_seq > ts->orphan_seq) ?
				 other->min_raw_indent : -1;

		if (min_indent >= 0 &&
		    (min_indent <= ts->target_root_indent ||
		     (ts->orphan_base_indent > 0 &&
		      min_indent <= ts->orphan_base_indent))) {
			ts->orphan_depth = 0;
			ts->orphan_base_indent = 0;
			ts->orphan_seq = 0;
		}
	}

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

		if (prune) {
			zfree(&frame->name);
			if (ts->in_target && i == ts->target_root_frame_idx)
				reset_target_root_state(ts);
		}
	}

	for (i = 0; i < ts->depth; i++) {
		if (ts->stack[i].name) {
			if (ts->in_target && i == ts->target_root_frame_idx) {
				ts->target_root_depth += dst - i;
				ts->target_root_frame_idx = dst;
			}
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
 * merge or replace it and update active CPU references accordingly.
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

	if (ts->in_target && ts->target_root_frame_idx >= to_copy)
		reset_target_root_state(ts);

	if (to_copy > 0) {
		int e_idx = 0, t_idx = 0, out_idx = 0;
		int new_e_root = -1, new_t_root = -1;

		if (orig_existing_depth > 0) {
			memmove(&existing->stack[to_copy],
				&existing->stack[0],
				orig_existing_depth * sizeof(existing->stack[0]));
		}

		while (e_idx < orig_existing_depth && t_idx < to_copy) {
			if (existing->stack[to_copy + e_idx].seq <=
			    ts->stack[t_idx].seq) {
				if (existing->in_target &&
				    e_idx == existing->target_root_frame_idx)
					new_e_root = out_idx;
				existing->stack[out_idx++] =
					existing->stack[to_copy + e_idx++];
			} else {
				if (ts->in_target &&
				    t_idx == ts->target_root_frame_idx)
					new_t_root = out_idx;
				existing->stack[out_idx++] =
					ts->stack[t_idx++];
			}
		}
		while (t_idx < to_copy) {
			if (ts->in_target &&
			    t_idx == ts->target_root_frame_idx)
				new_t_root = out_idx;
			existing->stack[out_idx++] = ts->stack[t_idx++];
		}
		while (e_idx < orig_existing_depth) {
			if (existing->in_target &&
			    e_idx == existing->target_root_frame_idx)
				new_e_root = out_idx;
			existing->stack[out_idx++] =
				existing->stack[to_copy + e_idx++];
		}

		if (new_t_root >= 0 &&
		    (new_e_root < 0 || new_t_root < new_e_root)) {
			int delta = new_t_root - ts->target_root_frame_idx;

			existing->in_target = true;
			existing->target_root_depth =
				ts->target_root_depth + delta;
			existing->target_root_frame_idx = new_t_root;
			existing->target_root_indent = ts->target_root_indent;
			strlcpy(existing->target_root_name,
				ts->target_root_name,
				sizeof(existing->target_root_name));
		} else if (new_e_root >= 0) {
			int delta = new_e_root - existing->target_root_frame_idx;

			existing->target_root_depth += delta;
			existing->target_root_frame_idx = new_e_root;
		}

		if (existing->in_target) {
			const struct ftrace_task_state *first =
				(ts->seq >= existing->seq) ? ts : existing;
			const struct ftrace_task_state *second =
				(ts->seq >= existing->seq) ? existing : ts;

			if (first->orphan_depth > 0) {
				existing->orphan_depth = first->orphan_depth;
				existing->orphan_base_indent = first->orphan_base_indent;
				existing->orphan_seq = first->orphan_seq;
			} else if (second->orphan_depth > 0) {
				existing->orphan_depth = second->orphan_depth;
				existing->orphan_base_indent = second->orphan_base_indent;
				existing->orphan_seq = second->orphan_seq;
			} else {
				existing->orphan_depth = 0;
				existing->orphan_base_indent = 0;
				existing->orphan_seq = 0;
			}

			if (ts->seq >= existing->seq) {
				strlcpy(existing->pending_irq_ent,
					ts->pending_irq_ent,
					sizeof(existing->pending_irq_ent));
				existing->suppress_irq_ret = ts->suppress_irq_ret;
			}
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
	 * inlined frames are preserved when it schedules back in.
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
 * ftrace_resolve_inlines - Resolve inlined functions for a return address
 * @machine: Perf machine context
 * @sym_name: Name of the caller function
 * @offset: Instruction offset within the caller function
 * @inlined_names: Array to store the resolved inline function names
 * @max_inlines: Maximum number of inline frames to resolve
 *
 * Use DWARF debug information from the kernel image to resolve a caller's
 * return address to its inlined callchain.
 *
 * Return: The number of inline functions successfully resolved.
 */
int ftrace_resolve_inlines(struct machine *machine,
			   const char *sym_name, u64 offset,
			   const char **inlined_names, int max_inlines)
{
	struct map *map = NULL;
	struct symbol *sym;
	struct dso *dso;
	struct inline_node *node;
	struct inline_list *ilist;
	u64 ip, addr;
	int count = 0;

	if (!machine || !offset)
		return 0;

	sym = machine__find_kernel_symbol_by_name(machine, sym_name, &map);
	if (!sym || !map)
		return 0;

	dso = map__dso(map);
	if (!dso) {
		map__put(map);
		return 0;
	}

	ip = sym->start + offset - 1;
	addr = map__rip_2objdump(map, ip);

	node = inlines__tree_find(dso__inlined_nodes(dso), addr);
	if (!node) {
		node = dso__parse_addr_inlines(dso, addr, sym);
		if (node)
			inlines__tree_insert(dso__inlined_nodes(dso), node);
	}

	if (!node) {
		map__put(map);
		return 0;
	}

	list_for_each_entry_reverse(ilist, &node->val, list) {
		if (!ilist->symbol || !symbol__inlined(ilist->symbol))
			continue;

		if (count < max_inlines)
			inlined_names[count++] = ilist->symbol->name;
	}

	map__put(map);
	return count;
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
 * push_nontarget_frame - Push non-target entry frame to call stack
 * @ts: Task stack state
 * @func_start: Pointer to start of function name in trace record
 * @leading_spaces: Indentation of trace record
 *
 * Return: 0 on success, or negative error code on failure.
 */
static int push_nontarget_frame(struct ftrace_task_state *ts, const char *func_start,
				int leading_spaces)
{
	char entry_name[128];

	extract_entry_func_name(func_start, entry_name, sizeof(entry_name));
	if (ts->depth >= ts->max_stack && ftrace_task_state__grow_stack(ts) < 0)
		return -ENOMEM;

	ts->stack[ts->depth].name = strdup(entry_name);
	if (!ts->stack[ts->depth].name) {
		pr_err("Not enough memory\n");
		return -ENOMEM;
	}
	ts->stack[ts->depth].inlined = false;
	ts->stack[ts->depth].indent = leading_spaces;
	ts->stack[ts->depth].seq = ts->seq;
	ts->depth++;
	return 0;
}

/**
 * make_blank_prefix - Create a blank prefix mirroring a trace record's context
 * @ftrace: Perf ftrace context
 * @line: The original trace record
 * @pipe_char: Pointer to the final delimiter character of the context columns
 * @buf: Buffer to store the blank prefix
 * @size: Size of the @buf
 *
 * Generate a prefix matching the context columns of @line (e.g. CPU, duration)
 * but with the duration field replaced by spaces.
 */
static void make_blank_prefix(const struct perf_ftrace *ftrace, const char *line,
			      const char *pipe_char, char *buf, size_t size)
{
	size_t len = pipe_char - line + 1;
	const char *start_delim = NULL;
	char *c;

	if (!size)
		return;

	if (len >= size)
		len = size - 1;

	memcpy(buf, line, len);
	buf[len] = '\0';

	if (ftrace->graph_noduration)
		return;

	if (*pipe_char == '|') {
		if (ftrace->graph_verbose) {
			/* In verbose mode, duration starts after the comm-pid '|' */
			for (const char *p = pipe_char - 1; p >= line; p--) {
				if (*p == '|') {
					start_delim = p;
					break;
				}
			}
		} else {
			/* In default mode, duration starts after CPU ')' */
			start_delim = strchr(line, ')');
		}

		if (start_delim && start_delim < pipe_char && start_delim < line + len) {
			char *end = buf + (pipe_char - line);

			if (end > buf + len)
				end = buf + len;
			for (c = buf + (start_delim - line) + 1; c < end; c++)
				*c = ' ';
		}
	}
}

/**
 * parse_graph_cpu - Extract CPU number from a function_graph trace record
 * @ftrace: Perf ftrace context
 * @line: The trace record to parse
 * @cpu: Pointer to store the CPU number
 *
 * Return: true if CPU was successfully parsed, false otherwise.
 */
static bool parse_graph_cpu(const struct perf_ftrace *ftrace,
			    const char *line, int *cpu)
{
	const char *p = line;
	char *endptr;
	long val;

	if (ftrace->graph_verbose) {
		p = strchr(p, '|');
		if (!p)
			return false;
		p++;
	}

	p = skip_spaces(p);
	val = strtol(p, &endptr, 10);
	if (endptr == p || *endptr != ')')
		return false;

	if (val < 0 || val >= FTRACE_MAX_CPUS)
		return false;

	if (ftrace->graph_verbose) {
		const char *proc_start = endptr;
		const char *pipe = strchr(proc_start, '|');

		if (pipe) {
			const char *h = pipe - 1;

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
	}

	*cpu = (int)val;
	return true;
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
 * verbose, whereas the duration column, and thus its '|' delimiter, is
 * omitted with --graph-opts noduration.
 *
 * Return: A pointer to the last character of the context columns (the final
 * '|' delimiter, or the CPU ')' when no delimiter follows it). Returns NULL
 * if @line is not a trace record (e.g. a header or a context switch marker).
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

	if (!ftrace->graph_noduration) {
		p = strchr(p + 1, '|');
		if (!p)
			return NULL;
	} else if (strstr(p, " => ") || strstr(p, "=====>") || strstr(p, "<=====")) {
		/* Context switch or interrupt marker */
		return NULL;
	}

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
 * ftrace_has_graph_funcs - Test whether graph target filters are active
 * @ftrace: Perf ftrace context
 *
 * Return: true if any target filter functions are configured, false otherwise.
 */
static inline bool ftrace_has_graph_funcs(const struct perf_ftrace *ftrace)
{
	return ftrace->graph_funcs.next != NULL && !list_empty(&ftrace->graph_funcs);
}

/**
 * ftrace_is_graph_func - Check if a function matches the graph_funcs target filter
 * @ftrace: Perf ftrace context
 * @func_name: The function name to test
 *
 * Return: true if ftrace->graph_funcs is empty or @func_name matches a
 * target filter (including glob and compiler clone matches), false otherwise.
 */
static bool ftrace_is_graph_func(const struct perf_ftrace *ftrace, const char *func_name)
{
	struct filter_entry *pos;
	char base_name[128];
	const char *dot;

	if (!ftrace_has_graph_funcs(ftrace))
		return true;

	/* Extract base symbol name by stripping compiler clone suffixes (.isra.*, .part.*, etc.) */
	dot = strchr(func_name, '.');
	if (dot) {
		size_t base_len = dot - func_name;

		if (base_len < sizeof(base_name)) {
			memcpy(base_name, func_name, base_len);
			base_name[base_len] = '\0';
		} else {
			dot = NULL;
		}
	}

	list_for_each_entry(pos, &ftrace->graph_funcs, list) {
		/* Fast path: non-glob patterns */
		if (!strisglob(pos->name)) {
			size_t len = strlen(pos->name);

			if (strcmp(pos->name, func_name) == 0)
				return true;
			if (!strncmp(pos->name, func_name, len) &&
			    (func_name[len] == '\0' || func_name[len] == '.'))
				return true;
			continue;
		}

		/* Glob pattern matching */
		if (strglobmatch(func_name, pos->name))
			return true;
		if (dot && strglobmatch(base_name, pos->name))
			return true;
	}

	return false;
}

/**
 * ftrace_reset_to_target_root - Initialise a new target root frame state
 * @ts: Task state to update
 * @entry_name: Function symbol name matching the graph filter
 * @depth: Call stack depth at which the target root function entered
 * @leading_spaces: Indentation of the target root function
 *
 * Mark the task as actively inside a target filter hierarchy rooted at @depth
 * for function @entry_name, and clear any stale orphan tracking depth.
 */
static void ftrace_reset_to_target_root(struct ftrace_task_state *ts,
					const char *entry_name, int depth,
					int leading_spaces)
{
	ts->in_target = true;
	ts->target_root_depth = depth;
	ts->target_root_frame_idx = depth;
	ts->target_root_indent = leading_spaces;
	strlcpy(ts->target_root_name, entry_name, sizeof(ts->target_root_name));
	ts->orphan_depth = 0;
	ts->orphan_base_indent = 0;
	ts->orphan_seq = 0;
	ts->pending_irq_ent[0] = '\0';
	ts->suppress_irq_ret = false;
}

/**
 * ftrace_unwind_to_depth - Unwind call stack frames down to a specified depth
 * @ts: Task state whose call stack is unwound
 * @target_depth: Depth to unwind down to
 * @blank_prefix: Column alignment prefix for synthesised closing lines
 * @out: File stream to output to, or NULL if suppressing output
 *
 * Pop and free call stack frames until the stack depth reaches @target_depth.
 * For any inlined frames encountered, decrement the inlined depth counter
 * and, if @out is non-NULL, emit the corresponding synthesised inline closing
 * delimiter line.
 */
static void ftrace_unwind_to_depth(struct ftrace_task_state *ts, int target_depth,
				   const char *blank_prefix, FILE *out)
{
	while (ts->depth > target_depth) {
		struct ftrace_stack_frame *top = &ts->stack[ts->depth - 1];

		if (top->inlined && out)
			fprintf(out, "%s%*s} /* %s (inline) */\n",
				blank_prefix, top->indent, "", top->name);
		free(top->name);
		ts->depth--;
	}
}

/**
 * ftrace_check_target_caller - Validate caller of function inside target filter
 * @ftrace: Perf ftrace context
 * @ts: Task stack state
 * @func_start: Pointer to start of function name in trace record
 * @entry_name: Function entry symbol name
 * @is_graph_target: Whether function matches target filter
 * @leading_spaces: Indentation of trace record
 * @has_open_brace: Whether record opens a frame
 * @has_semicolon: Whether record is a leaf call
 * @has_retaddr: Whether return address comment was parsed
 * @caller_idx: Index of caller on tracked stack, or -1 if untraced
 * @blank_prefix: Space prefix for synthesised closing lines
 * @out: File stream to output to
 * @is_target_root: Pointer to target root flag to update
 *
 * Return: 1 if line should be dropped (orphan or non-target ancestor),
 *         0 if entry should proceed as valid call or new target root,
 *         negative error code on memory failure.
 */
static int ftrace_check_target_caller(struct perf_ftrace *ftrace,
				      struct ftrace_task_state *ts,
				      const char *func_start,
				      const char *entry_name,
				      bool is_graph_target,
				      int leading_spaces,
				      bool has_open_brace,
				      bool has_semicolon,
				      bool has_retaddr,
				      int caller_idx,
				      const char *blank_prefix,
				      FILE *out,
				      bool *is_target_root)
{
	if (!ftrace_has_graph_funcs(ftrace) || !ts->in_target ||
	    ts->depth <= ts->target_root_depth)
		return 0;

	if (has_retaddr && caller_idx >= 0 && caller_idx < ts->target_root_depth) {
		ftrace_unwind_to_depth(ts, caller_idx + 1, blank_prefix, out);
		if (is_graph_target) {
			ftrace_reset_to_target_root(ts, entry_name, ts->depth,
						    leading_spaces);
			*is_target_root = true;
			return 0;
		}
		reset_target_root_state(ts);
		if (has_open_brace && !has_semicolon &&
		    push_nontarget_frame(ts, func_start, leading_spaces) < 0)
			return -ENOMEM;
		return 1;
	}

	if (leading_spaces <= ts->target_root_indent) {
		ftrace_unwind_to_depth(ts, 0, blank_prefix, out);
		if (is_graph_target) {
			ftrace_reset_to_target_root(ts, entry_name, 0,
						    leading_spaces);
			*is_target_root = true;
			return 0;
		}
		reset_target_root_state(ts);
		if (has_open_brace && !has_semicolon &&
		    push_nontarget_frame(ts, func_start, leading_spaces) < 0)
			return -ENOMEM;
		return 1;
	}

	if (has_retaddr) {
		if (caller_idx < 0) {
			if (ts->pending_irq_ent[0] != '\0') {
				ts->pending_irq_ent[0] = '\0';
				ts->suppress_irq_ret = true;
			}
			if (has_open_brace && !has_semicolon) {
				ts->orphan_depth = 1;
				ts->orphan_base_indent = leading_spaces;
				ts->orphan_seq = ts->seq;
			}
			return 1;
		}
	}

	return 0;
}

/**
 * ftrace_process_fgraph_line - Process and output a function graph trace line
 * @ftrace: Perf ftrace context
 * @line: The trace line to process
 * @out: File stream to output to
 *
 * Process a trace line from the function_graph tracer. If --inline is set,
 * this function parses the return address comment, resolves it to its
 * inlined callchain, and synthesises intermediate frames into the streamed
 * call graph.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int ftrace_process_fgraph_line(struct perf_ftrace *ftrace, const char *line, FILE *out)
{
	char caller_sym[128];
	char blank_prefix[128];
	const char *inlined_names[FTRACE_MAX_INLINES];
	const char *pipe_char;
	const char *after_pipe;
	const char *func_start;
	struct ftrace_task_state *ts;
	u64 caller_offset = 0;
	int num_inlines = 0;
	int cpu = -1;
	int leading_spaces;
	int extra_indent;
	int caller_idx;
	int active_inlines;
	int common;
	int match_idx;
	int ret;
	int i;
	char entry_name[128];
	bool is_exit;
	bool has_open_brace;
	bool has_semicolon;
	bool has_retaddr = false;
	bool has_filter = ftrace_has_graph_funcs(ftrace);
	bool is_graph_target = false;
	bool is_target_root = false;
	bool was_target_root = false;

	if ((!has_filter || ftrace->graph_thresh) && !ftrace->use_inline)
		return print_line(ftrace, line, out);

	if (strstr(line, "==========>") != NULL ||
	    strstr(line, "<==========") != NULL) {
		bool is_irq_ent = (strstr(line, "==========>") != NULL);

		if (has_filter && parse_graph_cpu(ftrace, line, &cpu)) {
			ts = get_cpu_state(ftrace, cpu);
			if (ts && ts->in_target && ts->orphan_depth == 0) {
				if (is_irq_ent) {
					if (ts->pending_irq_ent[0] != '\0')
						print_line(ftrace, ts->pending_irq_ent, out);
					strlcpy(ts->pending_irq_ent, line,
						sizeof(ts->pending_irq_ent));
					ts->suppress_irq_ret = false;
				} else {
					if (ts->pending_irq_ent[0] != '\0') {
						print_line(ftrace, ts->pending_irq_ent, out);
						ts->pending_irq_ent[0] = '\0';
					}
					if (ts->suppress_irq_ret)
						ts->suppress_irq_ret = false;
					else
						print_line(ftrace, line, out);
				}
			} else if (ts && !ts->in_target) {
				ts->pending_irq_ent[0] = '\0';
				ts->suppress_irq_ret = false;
			}
		} else if (!has_filter) {
			print_line(ftrace, line, out);
		}
		return 0;
	}

	pipe_char = find_func_column(ftrace, line, &cpu);
	if (!pipe_char) {
		parse_context_switch(line);
		return print_line(ftrace, line, out);
	}

	ts = get_cpu_state(ftrace, cpu);
	if (!ts)
		return print_line(ftrace, line, out);

	ts->seq = ++ftrace_event_seq;
	make_blank_prefix(ftrace, line, pipe_char, blank_prefix, sizeof(blank_prefix));

	after_pipe = pipe_char + 1;
	func_start = skip_spaces(after_pipe);
	leading_spaces = func_start - after_pipe;

	is_exit = (*func_start == '}');

	if (*func_start == '\0' ||
	    (!isalpha(*func_start) && *func_start != '_' && !is_exit))
		return print_line(ftrace, line, out);

	if (is_exit) {
		char exit_name[128];

		ftrace_task_state__record_indent(ts, leading_spaces);

		extract_exit_func_name(func_start, exit_name, sizeof(exit_name));

		if (ts->orphan_depth > 0) {
			if (has_filter && ts->in_target &&
			    (leading_spaces <= ts->target_root_indent ||
			     (ts->orphan_base_indent > 0 &&
			      leading_spaces < ts->orphan_base_indent))) {
				ts->orphan_depth = 0;
				ts->orphan_base_indent = 0;
				ts->orphan_seq = 0;
			} else {
				if (ts->orphan_base_indent > 0 &&
				    leading_spaces == ts->orphan_base_indent)
					ts->orphan_depth = 0;
				else
					ts->orphan_depth--;
				if (ts->orphan_depth == 0) {
					ts->orphan_base_indent = 0;
					ts->orphan_seq = 0;
				}
				return 0;
			}
		}

		ts->suppress_irq_ret = false;

		/* Find matching frame on stack if exit is named */
		match_idx = -1;
		if (exit_name[0] != '\0') {
			for (i = ts->depth - 1; i >= 0; i--) {
				if (!ts->stack[i].inlined && ts->stack[i].name &&
				    ts->stack[i].indent == leading_spaces &&
				    strcmp(ts->stack[i].name, exit_name) == 0) {
					match_idx = i;
					break;
				}
			}
		}

		/*
		 * If bare brace ('}'), match the topmost non-inlined frame
		 * with matching indentation.
		 */
		if (match_idx < 0 && exit_name[0] == '\0') {
			for (i = ts->depth - 1; i >= 0; i--) {
				if (!ts->stack[i].inlined &&
				    ts->stack[i].indent == leading_spaces) {
					match_idx = i;
					break;
				}
			}
		}

		if (ts->pending_irq_ent[0] != '\0') {
			if (!has_filter || (ts->in_target && match_idx >= ts->target_root_depth))
				print_line(ftrace, ts->pending_irq_ent, out);
			ts->pending_irq_ent[0] = '\0';
		}

		if (match_idx >= 0) {
			was_target_root = (has_filter && ts->in_target &&
					   match_idx == ts->target_root_frame_idx);

			/* Close all inlined and orphaned frames above match_idx */
			while (ts->depth > 0 && ts->depth - 1 > match_idx) {
				struct ftrace_stack_frame *top = &ts->stack[ts->depth - 1];

				if (top->inlined) {
					int exit_indent = top->indent;

					if (!has_filter || (ts->in_target &&
					    ts->depth - 1 >= ts->target_root_depth))
						fprintf(out, "%s%*s} /* %s (inline) */\n",
							blank_prefix, exit_indent, "", top->name);
				}
				free(top->name);
				ts->depth--;
			}

			/* Pop the matched real frame */
			if (ts->depth > 0 && match_idx == ts->depth - 1 &&
			    !ts->stack[ts->depth - 1].inlined) {
				free(ts->stack[ts->depth - 1].name);
				ts->depth--;
			}

			extra_indent = ftrace_task_state__inlined_depth(ts) * 2;
		} else {
			extra_indent = 0;
		}

		if (!has_filter || (ts->in_target && match_idx >= ts->target_root_depth)) {
			if (!ftrace->use_inline) {
				print_line(ftrace, line, out);
			} else {
				fprintf(out, "%.*s%*s%s\n",
					(int)(pipe_char - line + 1), line,
					leading_spaces + extra_indent, "", func_start);
			}
		}

		if (has_filter && ts->in_target) {
			if (was_target_root) {
				while (ts->depth > ts->target_root_depth &&
				       ts->stack[ts->depth - 1].inlined) {
					struct ftrace_stack_frame *top =
						&ts->stack[ts->depth - 1];
					int exit_indent = top->indent;

					fprintf(out, "%s%*s} /* %s (inline) */\n",
						blank_prefix, exit_indent, "", top->name);
					free(top->name);
					ts->depth--;
				}
				reset_target_root_state(ts);
			} else if (ts->depth <= ts->target_root_depth) {
				reset_target_root_state(ts);
			} else if (leading_spaces <= ts->target_root_indent) {
				ftrace_unwind_to_depth(ts, ts->target_root_depth,
						       blank_prefix, out);
				reset_target_root_state(ts);
			}
		}
		if (has_filter && !ts->in_target && match_idx < 0) {
			while (ts->depth > 0 &&
			       (ts->stack[ts->depth - 1].inlined ||
				ts->stack[ts->depth - 1].indent >= leading_spaces)) {
				free(ts->stack[ts->depth - 1].name);
				ts->depth--;
			}
		}
		return 0;
	}

	/* It's an entry or a leaf call */
	has_open_brace = (strchr(func_start, '{') != NULL);
	has_semicolon = (strchr(func_start, ';') != NULL);

	extract_entry_func_name(func_start, entry_name, sizeof(entry_name));
	if (has_filter)
		is_graph_target = ftrace_is_graph_func(ftrace, entry_name);

	caller_idx = -1;
	if (ftrace_parse_retaddr(func_start, caller_sym, sizeof(caller_sym), &caller_offset)) {
		has_retaddr = true;
		for (i = ts->depth - 1; i >= 0; i--) {
			if (!ts->stack[i].inlined && ts->stack[i].name &&
			    ts->stack[i].indent < leading_spaces &&
			    strcmp(ts->stack[i].name, caller_sym) == 0) {
				caller_idx = i;
				break;
			}
		}
	}

	if (ts->orphan_depth > 0) {
		/*
		 * If an orphan exit was dropped by the kernel ring buffer,
		 * check whether this entry is a new target root function,
		 * is shallower than the orphan root, or descends directly
		 * from an active frame on the tracked call stack.
		 */
		if (has_filter && ts->in_target) {
			if (leading_spaces <= ts->target_root_indent ||
			    (ts->orphan_base_indent > 0 &&
			     leading_spaces < ts->orphan_base_indent)) {
				ts->orphan_depth = 0;
				ts->orphan_base_indent = 0;
				ts->orphan_seq = 0;
			} else if (has_retaddr && caller_idx >= 0 &&
				   leading_spaces == ts->stack[caller_idx].indent + 2 &&
				   (ts->orphan_base_indent == 0 ||
				    leading_spaces <= ts->orphan_base_indent)) {
				ts->orphan_depth = 0;
				ts->orphan_base_indent = 0;
				ts->orphan_seq = 0;
			} else if (ts->target_root_name[0] != '\0' && has_retaddr &&
				   leading_spaces == ts->target_root_indent + 2) {
				if (strcmp(ts->target_root_name, caller_sym) == 0) {
					ts->orphan_depth = 0;
					ts->orphan_base_indent = 0;
					ts->orphan_seq = 0;
				}
			}
		}

		if (ts->orphan_depth > 0) {
			if (has_open_brace && !has_semicolon) {
				ts->orphan_depth++;
				ts->orphan_seq = ts->seq;
			}
			return 0;
		}
	}

	ts->suppress_irq_ret = false;

	if ((has_open_brace || has_semicolon) &&
	    !(has_filter && ts->in_target && ts->depth > ts->target_root_depth &&
	      leading_spaces > ts->target_root_indent &&
	      has_retaddr && caller_idx < 0))
		ftrace_task_state__record_indent(ts, leading_spaces);

	if (has_filter) {
		if (ts->depth == 0 || !ts->in_target) {
			if (is_graph_target) {
				if (has_retaddr && caller_idx >= 0)
					ftrace_unwind_to_depth(ts, caller_idx + 1,
							       blank_prefix,
							       ftrace->use_inline ? out : NULL);
				else
					ftrace_unwind_to_depth(ts, 0,
							       blank_prefix,
							       ftrace->use_inline ? out : NULL);
				ftrace_reset_to_target_root(ts, entry_name, ts->depth,
							    leading_spaces);
				is_target_root = true;
			} else if (ts->depth == 0) {
				reset_target_root_state(ts);
			}
		}

		if (!ts->in_target) {
			/* Non-matching call tree; track depth if entering scope, suppress output */
			while (ts->depth > 0 &&
			       !ts->stack[ts->depth - 1].inlined &&
			       ts->stack[ts->depth - 1].indent >= leading_spaces) {
				free(ts->stack[ts->depth - 1].name);
				ts->depth--;
			}
			if (has_open_brace && !has_semicolon) {
				if (push_nontarget_frame(ts, func_start, leading_spaces) < 0)
					return -ENOMEM;
			}
			return 0;
		}
	}

	if (!ftrace->use_inline) {
		if (is_target_root) {
			if (has_retaddr && caller_idx >= 0)
				ftrace_unwind_to_depth(ts, caller_idx + 1, NULL, NULL);
			else
				ftrace_unwind_to_depth(ts, 0, NULL, NULL);
			ts->target_root_depth = ts->depth;
			ts->target_root_frame_idx = ts->depth;
			ts->target_root_indent = leading_spaces;
		}

		ret = ftrace_check_target_caller(ftrace, ts, func_start, entry_name,
						 is_graph_target, leading_spaces,
						 has_open_brace, has_semicolon,
						 has_retaddr, caller_idx, NULL, NULL,
						 &is_target_root);
		if (ret != 0)
			return (ret < 0) ? ret : 0;

		if (!is_target_root) {
			if (has_retaddr && caller_idx >= 0) {
				if (ts->depth > caller_idx + 1)
					ftrace_unwind_to_depth(ts, caller_idx + 1, NULL, NULL);
			} else if (caller_idx < 0) {
				while (ts->depth > ts->target_root_depth &&
				       !ts->stack[ts->depth - 1].inlined &&
				       ts->stack[ts->depth - 1].indent >= leading_spaces) {
					free(ts->stack[ts->depth - 1].name);
					ts->depth--;
				}
			}
		}

		if (has_open_brace && !has_semicolon) {
			if (is_target_root) {
				ts->target_root_frame_idx = ts->depth;
				ts->target_root_indent = leading_spaces;
			}
			if (push_nontarget_frame(ts, func_start, leading_spaces) < 0)
				return -ENOMEM;
		}
		if (ts->pending_irq_ent[0] != '\0') {
			print_line(ftrace, ts->pending_irq_ent, out);
			ts->pending_irq_ent[0] = '\0';
		}
		print_line(ftrace, line, out);
		if (has_filter && ts->in_target && is_target_root &&
		    has_semicolon && !has_open_brace) {
			reset_target_root_state(ts);
		}
		return 0;
	}

	if (has_retaddr) {
		num_inlines = ftrace_resolve_inlines(ftrace->machine, caller_sym,
						      caller_offset, inlined_names,
						      FTRACE_MAX_INLINES);
		if (num_inlines < 0)
			return num_inlines;

		/* If caller is not on the tracked stack, do not synthesise inlines */
		if (caller_idx < 0)
			num_inlines = 0;
	}

	ret = ftrace_check_target_caller(ftrace, ts, func_start, entry_name,
					 is_graph_target, leading_spaces,
					 has_open_brace, has_semicolon,
					 has_retaddr, caller_idx, blank_prefix,
					 out, &is_target_root);
	if (ret != 0)
		return (ret < 0) ? ret : 0;

	if (ts->pending_irq_ent[0] != '\0') {
		print_line(ftrace, ts->pending_irq_ent, out);
		ts->pending_irq_ent[0] = '\0';
	}

	active_inlines = 0;
	if (caller_idx >= 0) {
		for (i = caller_idx + 1; i < ts->depth; i++) {
			if (ts->stack[i].inlined)
				active_inlines++;
			else
				break;
		}
	}

	/* Find common prefix of inlined functions */
	common = 0;
	if (caller_idx >= 0) {
		while (common < active_inlines && common < num_inlines &&
		       ts->stack[caller_idx + 1 + common].name &&
		       !strcmp(ts->stack[caller_idx + 1 + common].name,
			       inlined_names[common])) {
			common++;
		}
	}

	/* Close stale callee frames and inactive inlined frames above caller */
	while (caller_idx >= 0 && ts->depth > caller_idx + 1 + common) {
		struct ftrace_stack_frame *top = &ts->stack[ts->depth - 1];

		if (top->inlined) {
			int exit_indent = top->indent;

			if (!has_filter || (ts->in_target && !is_target_root))
				fprintf(out, "%s%*s} /* %s (inline) */\n",
					blank_prefix, exit_indent, "", top->name);
		}
		free(top->name);
		ts->depth--;
	}

	if (caller_idx < 0) {
		if (is_target_root) {
			ftrace_unwind_to_depth(ts, 0, blank_prefix, out);
		} else {
			/*
			 * If caller is not on the tracked stack, unwind any frames whose
			 * raw indentation is greater than or equal to the new entry's
			 * indentation (their exit events were dropped), and close any
			 * trailing inlined frames that were left open from prior calls.
			 */
			while (ts->depth > 0) {
				struct ftrace_stack_frame *top = &ts->stack[ts->depth - 1];

				if (top->inlined) {
					int exit_indent = top->indent;

					if (!has_filter || (ts->in_target && !is_target_root))
						fprintf(out, "%s%*s} /* %s (inline) */\n",
							blank_prefix, exit_indent, "", top->name);
					free(top->name);
					ts->depth--;
				} else if (top->indent >= leading_spaces) {
					free(top->name);
					ts->depth--;
				} else {
					break;
				}
			}
		}
	}

	if (is_target_root) {
		ts->target_root_depth = ts->depth;
		ts->target_root_indent = leading_spaces;
	}

	/* Open new inlined frames */
	for (i = common; i < num_inlines; i++) {
		int entry_indent = leading_spaces + ftrace_task_state__inlined_depth(ts) * 2;

		if (!inlined_names[i]) {
			pr_err("Not enough memory\n");
			return -ENOMEM;
		}
		if (ts->depth >= ts->max_stack &&
		    ftrace_task_state__grow_stack(ts) < 0)
			break;

		fprintf(out, "%s%*s%s() { /* (inline) */\n",
			blank_prefix, entry_indent, "", inlined_names[i]);
		ts->stack[ts->depth].name = strdup(inlined_names[i]);
		if (!ts->stack[ts->depth].name) {
			pr_err("Not enough memory\n");
			return -ENOMEM;
		}
		ts->stack[ts->depth].inlined = true;
		ts->stack[ts->depth].indent = entry_indent;
		ts->stack[ts->depth].seq = ts->seq;
		ts->depth++;
	}

	/* Print current line with updated indentation */
	extra_indent = ftrace_task_state__inlined_depth(ts) * 2;

	if (!ftrace->graph_retaddr) {
		char *filtered = strdup(func_start);

		if (filtered) {
			ftrace_filter_retaddr(filtered);
			fprintf(out, "%.*s%*s%s\n",
				(int)(pipe_char - line + 1), line,
				leading_spaces + extra_indent, "", filtered);
			free(filtered);
		} else {
			pr_err("Not enough memory\n");
			return -ENOMEM;
		}
	} else {
		fprintf(out, "%.*s%*s%s\n",
			(int)(pipe_char - line + 1), line,
			leading_spaces + extra_indent, "", func_start);
	}

	/* If non-leaf entry, push onto stack */
	if (has_open_brace && !has_semicolon) {
		if (ts->depth >= ts->max_stack &&
		    ftrace_task_state__grow_stack(ts) < 0)
			return -ENOMEM;

		if (is_target_root) {
			ts->target_root_frame_idx = ts->depth;
			ts->target_root_indent = leading_spaces;
		}

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

	if (has_filter && ts->in_target && is_target_root &&
	    has_semicolon && !has_open_brace) {
		while (ts->depth > ts->target_root_depth) {
			struct ftrace_stack_frame *top = &ts->stack[ts->depth - 1];

			if (top->inlined) {
				int exit_indent = top->indent;

				fprintf(out, "%s%*s} /* %s (inline) */\n",
					blank_prefix, exit_indent, "", top->name);
			}
			free(top->name);
			ts->depth--;
		}
		reset_target_root_state(ts);
	}

	return 0;
}

/**
 * perf_ftrace__setup_inlines - Initialise symbol resolution for inlines
 * @ftrace: Perf ftrace context
 *
 * Initialise symbol tracking and load the kernel image (vmlinux) for
 * DWARF debug information parsing.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int perf_ftrace__setup_inlines(struct perf_ftrace *ftrace)
{
	struct map *kmap;
	struct ftrace_task_state *ts;
	pid_t single_pid;
	int c;

	if (!ftrace->use_inline)
		return 0;

	single_pid = ftrace__get_single_pid(ftrace);
	if (single_pid > 0) {
		ts = get_task_state(single_pid);
		if (ts) {
			for (c = 0; c < FTRACE_MAX_CPUS; c++)
				ftrace_cpu_tasks[c] = ts;
		}
	}

	symbol_conf.try_vmlinux_path = (symbol_conf.vmlinux_name == NULL);
	symbol_conf.inline_name = true;
	if (symbol_conf.vmlinux_name)
		symbol_conf.ignore_vmlinux_buildid = true;

	if (symbol__init(NULL) < 0) {
		pr_warning("Failed to initialize symbols for inlined functions\n");
		return -1;
	}

	ftrace->machine = machine__new_host(NULL);
	if (!ftrace->machine) {
		pr_warning("Failed to create host machine for symbols\n");
		return -1;
	}

	if (symbol_conf.vmlinux_name) {
		kmap = machine__kernel_map(ftrace->machine);
		if (!kmap ||
		    dso__load_vmlinux(map__dso(kmap), kmap,
				      symbol_conf.vmlinux_name, false) <= 0) {
			pr_warning("Failed to load kernel image '%s'\n", symbol_conf.vmlinux_name);
			goto out_delete_machine;
		}
	} else {
		if (machine__load_vmlinux_path(ftrace->machine) <= 0) {
			pr_warning("Failed to load vmlinux from search path\n");
			goto out_delete_machine;
		}
	}

	return 0;

out_delete_machine:
	machine__delete(ftrace->machine);
	ftrace->machine = NULL;
	return -1;
}

/**
 * perf_ftrace__cleanup_inlines - Clean up resources used for inline resolution
 * @ftrace: Perf ftrace context
 *
 * Free the machine context and the CPU stack tracking states.
 */
void perf_ftrace__cleanup_inlines(struct perf_ftrace *ftrace)
{
	if (ftrace->machine) {
		machine__delete(ftrace->machine);
		ftrace->machine = NULL;
	}
	ftrace_cpu_states__clear();
}
