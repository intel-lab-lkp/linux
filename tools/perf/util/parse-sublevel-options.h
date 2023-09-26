#ifndef _PERF_PARSE_SUBLEVEL_OPTIONS_H
#define _PERF_PARSE_SUBLEVEL_OPTIONS_H

struct sublevel_option {
	const char *name;

	/*
	 * Only one of below can be non-null. So we simply support
	 * two types: integer and string. For string, the caller is
	 * responsible for freeing allocated memory after use.
	 */
	int *value_ptr;
	char **str_ptr;
};

int perf_parse_sublevel_options(const char *str, struct sublevel_option *opts);

#endif
