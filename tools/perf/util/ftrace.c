// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "util/ftrace.h"

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
