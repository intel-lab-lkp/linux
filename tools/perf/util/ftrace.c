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
#include <linux/kernel.h>
#include <linux/string.h>
#include "debug.h"
#include "dso.h"
#include "machine.h"
#include "map.h"
#include "srcline.h"
#include "symbol.h"
#include "util/ftrace.h"

#define FTRACE_MAX_CPUS 4096
#define FTRACE_MAX_STACK 256
#define FTRACE_MAX_INLINES 32

struct ftrace_stack_frame {
	char *name;
	bool inlined;
	int indent;
};

struct ftrace_cpu_state {
	struct ftrace_stack_frame stack[FTRACE_MAX_STACK];
	int depth;
	int inlined_depth;
};

static struct ftrace_cpu_state *ftrace_cpu_states[FTRACE_MAX_CPUS];

void ftrace_cpu_states__clear(void)
{
	int i, j;

	for (i = 0; i < FTRACE_MAX_CPUS; i++) {
		if (ftrace_cpu_states[i]) {
			for (j = 0; j < ftrace_cpu_states[i]->depth; j++)
				free(ftrace_cpu_states[i]->stack[j].name);
			free(ftrace_cpu_states[i]);
			ftrace_cpu_states[i] = NULL;
		}
	}
}

static struct ftrace_cpu_state *get_cpu_state(int cpu)
{
	if (cpu < 0 || cpu >= FTRACE_MAX_CPUS)
		return NULL;

	if (!ftrace_cpu_states[cpu])
		ftrace_cpu_states[cpu] = zalloc(sizeof(struct ftrace_cpu_state));

	return ftrace_cpu_states[cpu];
}

bool ftrace_parse_retaddr(const char *str, char *sym_name, size_t sym_len, u64 *offset)
{
	const char *p = strstr(str, "<-");
	const char *plus;
	const char *bracket;
	char *endptr;
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

	*offset = strtoull(plus + 1, &endptr, 16);
	return true;
}

void ftrace_filter_retaddr(char *str)
{
	char *start = strstr(str, "/* <-");
	char *end;

	if (!start)
		return;

	end = strstr(start, "*/");
	if (!end)
		return;

	end += 2; /* skip end-of-comment delimiter */

	/* Also backtrack any spaces before comment */
	while (start > str && *(start - 1) == ' ')
		start--;

	memmove(start, end, strlen(end) + 1);
}

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

	if (!machine)
		return 0;

	sym = machine__find_kernel_symbol_by_name(machine, sym_name, &map);
	if (!sym || !map)
		return 0;

	dso = map__dso(map);
	if (!dso) {
		map__put(map);
		return 0;
	}

	ip = sym->start + offset;
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

	list_for_each_entry(ilist, &node->val, list) {
		if (ilist->symbol && symbol__inlined(ilist->symbol)) {
			if (count < max_inlines) {
				char *name = strdup(ilist->symbol->name);

				if (name)
					inlined_names[count++] = name;
			}
		}
	}

	map__put(map);
	return count;
}

static void extract_entry_func_name(const char *str, char *buf, size_t size)
{
	size_t i = 0;

	while (*str && *str != '(' && *str != '{' && !isspace(*str) && i < size - 1)
		buf[i++] = *str++;
	buf[i] = '\0';
}

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

static void make_blank_prefix(const char *line, const char *pipe_char, char *buf, size_t size)
{
	size_t len = pipe_char - line + 1;
	const char *paren = strchr(line, ')');
	char *c;

	if (len >= size)
		len = size - 1;

	memcpy(buf, line, len);
	buf[len] = '\0';

	/* Replace duration between ')' and '|' with spaces */
	if (paren && paren < line + len) {
		char *end = buf + (pipe_char - line);

		if (end > buf + len)
			end = buf + len;
		for (c = buf + (paren - line) + 1; c < end; c++)
			*c = ' ';
	}
}

int ftrace_process_fgraph_line(struct perf_ftrace *ftrace, const char *line, FILE *out)
{
	char caller_sym[128];
	char blank_prefix[128];
	const char *inlined_names[FTRACE_MAX_INLINES];
	const char *pipe_char;
	const char *paren;
	const char *after_pipe;
	const char *func_start;
	struct ftrace_cpu_state *cs;
	u64 caller_offset = 0;
	int num_inlines = 0;
	int cpu = -1;
	int leading_spaces;
	int extra_indent;
	int caller_idx;
	int active_inlines;
	int common;
	int match_idx;
	int i;
	bool is_exit;
	bool has_open_brace;
	bool has_semicolon;

	if (!ftrace->use_inline) {
		if (ftrace->filter_retaddr) {
			char *filtered = strdup(line);

			if (filtered) {
				ftrace_filter_retaddr(filtered);
				fprintf(out, "%s\n", filtered);
				free(filtered);
			} else {
				pr_err("Not enough memory\n");
				return -ENOMEM;
			}
			return 0;
		}
		fprintf(out, "%s\n", line);
		return 0;
	}

	pipe_char = strchr(line, '|');
	if (!pipe_char) {
		fprintf(out, "%s\n", line);
		return 0;
	}

	/* Extract CPU number */
	paren = strchr(line, ')');
	if (paren && paren > line) {
		char *endptr;
		long val = strtol(skip_spaces(line), &endptr, 10);

		if (endptr == paren)
			cpu = (int)val;
	}

	cs = get_cpu_state(cpu);
	if (!cs) {
		fprintf(out, "%s\n", line);
		return 0;
	}

	make_blank_prefix(line, pipe_char, blank_prefix, sizeof(blank_prefix));

	after_pipe = pipe_char + 1;
	func_start = skip_spaces(after_pipe);
	leading_spaces = func_start - after_pipe;

	if (*func_start == '\0') {
		fprintf(out, "%s\n", line);
		return 0;
	}

	is_exit = (*func_start == '}');

	if (is_exit) {
		char exit_name[128];

		extract_exit_func_name(func_start, exit_name, sizeof(exit_name));

		/* Find matching frame on stack */
		match_idx = -1;
		for (i = cs->depth - 1; i >= 0; i--) {
			if (!cs->stack[i].inlined) {
				if (exit_name[0] == '\0' ||
				    (cs->stack[i].name &&
				     strcmp(cs->stack[i].name, exit_name) == 0)) {
					match_idx = i;
					break;
				}
			}
		}

		/* Close all inlined frames above match_idx */
		while (cs->depth > 0 && (match_idx < 0 || cs->depth - 1 > match_idx)) {
			if (cs->stack[cs->depth - 1].inlined) {
				struct ftrace_stack_frame *top = &cs->stack[cs->depth - 1];
				int exit_indent = top->indent;

				if (cs->inlined_depth > 0)
					cs->inlined_depth--;
				fprintf(out, "%s%*s} /* %s (inline) */\n",
					blank_prefix, exit_indent, "", top->name);
				free(top->name);
				cs->depth--;
			} else {
				break;
			}
		}

		/* Pop the matched real frame */
		if (cs->depth > 0 && match_idx == cs->depth - 1 &&
		    !cs->stack[cs->depth - 1].inlined) {
			free(cs->stack[cs->depth - 1].name);
			cs->depth--;
		}

		/* Emit the exit line with extra indentation if within inlined functions */
		extra_indent = cs->inlined_depth * 2;

		fprintf(out, "%.*s%*s%s\n",
			(int)(pipe_char - line + 1), line,
			leading_spaces + extra_indent, "", func_start);
		return 0;
	}

	/* It's an entry or a leaf call */
	has_open_brace = (strchr(func_start, '{') != NULL);
	has_semicolon = (strchr(func_start, ';') != NULL);

	if (ftrace_parse_retaddr(func_start, caller_sym, sizeof(caller_sym), &caller_offset)) {
		num_inlines = ftrace_resolve_inlines(ftrace->machine, caller_sym,
						      caller_offset, inlined_names,
						      FTRACE_MAX_INLINES);
	}

	/* Determine which inlined frames are currently open above the caller */
	caller_idx = -1;
	if (num_inlines > 0) {
		for (i = cs->depth - 1; i >= 0; i--) {
			if (!cs->stack[i].inlined && cs->stack[i].name &&
			    strcmp(cs->stack[i].name, caller_sym) == 0) {
				caller_idx = i;
				break;
			}
		}
	}

	active_inlines = 0;
	if (caller_idx >= 0) {
		for (i = caller_idx + 1; i < cs->depth; i++) {
			if (cs->stack[i].inlined)
				active_inlines++;
			else
				break;
		}
	}

	/* Find common prefix of inlined functions */
	common = 0;
	if (caller_idx >= 0) {
		while (common < active_inlines && common < num_inlines &&
		       cs->stack[caller_idx + 1 + common].name &&
		       !strcmp(cs->stack[caller_idx + 1 + common].name,
			       inlined_names[common])) {
			common++;
		}
	}

	/* Close inlined frames that are no longer active */
	while (active_inlines > common && cs->depth > 0) {
		if (cs->stack[cs->depth - 1].inlined) {
			struct ftrace_stack_frame *top = &cs->stack[cs->depth - 1];
			int exit_indent = top->indent;

			if (cs->inlined_depth > 0)
				cs->inlined_depth--;
			fprintf(out, "%s%*s} /* %s (inline) */\n",
				blank_prefix, exit_indent, "", top->name);
			free(top->name);
			cs->depth--;
			active_inlines--;
		} else {
			break;
		}
	}

	/* Open new inlined frames */
	for (i = common; i < num_inlines; i++) {
		int entry_indent = leading_spaces + cs->inlined_depth * 2;

		if (!inlined_names[i]) {
			pr_err("Not enough memory\n");
			for (int j = 0; j < num_inlines; j++)
				free((void *)inlined_names[j]);
			return -ENOMEM;
		}
		if (cs->depth >= FTRACE_MAX_STACK)
			break;

		fprintf(out, "%s%*s%s() { /* (inline) */\n",
			blank_prefix, entry_indent, "", inlined_names[i]);
		cs->stack[cs->depth].name = strdup(inlined_names[i]);
		if (!cs->stack[cs->depth].name) {
			pr_err("Not enough memory\n");
			for (int j = 0; j < num_inlines; j++)
				free((void *)inlined_names[j]);
			return -ENOMEM;
		}
		cs->stack[cs->depth].inlined = true;
		cs->stack[cs->depth].indent = entry_indent;
		cs->depth++;
		cs->inlined_depth++;
	}

	/* Print current line with updated indentation */
	extra_indent = cs->inlined_depth * 2;

	if (ftrace->filter_retaddr) {
		char *filtered = strdup(func_start);

		if (filtered) {
			ftrace_filter_retaddr(filtered);
			fprintf(out, "%.*s%*s%s\n",
				(int)(pipe_char - line + 1), line,
				leading_spaces + extra_indent, "", filtered);
			free(filtered);
		} else {
			pr_err("Not enough memory\n");
			for (i = 0; i < num_inlines; i++)
				free((void *)inlined_names[i]);
			return -ENOMEM;
		}
	} else {
		fprintf(out, "%.*s%*s%s\n",
			(int)(pipe_char - line + 1), line,
			leading_spaces + extra_indent, "", func_start);
	}

	/* If non-leaf entry, push onto stack */
	if (has_open_brace && !has_semicolon) {
		char entry_name[128];

		extract_entry_func_name(func_start, entry_name, sizeof(entry_name));
		if (cs->depth < FTRACE_MAX_STACK) {
			cs->stack[cs->depth].name = strdup(entry_name);
			if (!cs->stack[cs->depth].name) {
				pr_err("Not enough memory\n");
				for (i = 0; i < num_inlines; i++)
					free((void *)inlined_names[i]);
				return -ENOMEM;
			}
			cs->stack[cs->depth].inlined = false;
			cs->stack[cs->depth].indent = leading_spaces + extra_indent;
			cs->depth++;
		}
	}

	for (i = 0; i < num_inlines; i++)
		free((void *)inlined_names[i]);
	return 0;
}

int perf_ftrace__setup_inlines(struct perf_ftrace *ftrace)
{
	struct map *kmap;

	if (!ftrace->use_inline)
		return 0;

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
		if (kmap)
			dso__load_vmlinux(map__dso(kmap), kmap, symbol_conf.vmlinux_name, false);
	} else {
		machine__load_vmlinux_path(ftrace->machine);
	}

	return 0;
}

void perf_ftrace__cleanup_inlines(struct perf_ftrace *ftrace)
{
	if (ftrace->machine) {
		machine__delete(ftrace->machine);
		ftrace->machine = NULL;
	}
	ftrace_cpu_states__clear();
}
