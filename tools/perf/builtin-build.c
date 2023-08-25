// SPDX-License-Identifier: GPL-2.0
#include "builtin.h"
#include "color.h"
#include "util/debug.h"
#include "util/header.h"
#include <tools/config.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <subcmd/parse-options.h>

struct build {
	const char *has;
};

static struct build build;

static struct option build_options[] = {
	OPT_STRING(0, "has", &build.has, NULL, "check if a feature is built in"),
	OPT_END(),
};

static const char * const build_usage[] = {
	"perf build [<options>]",
	NULL
};

static void on_off_print(const char *status)
{
	printf("[ ");

	if (!strcmp(status, "OFF"))
		color_fprintf(stdout, PERF_COLOR_RED, "%-3s", status);
	else
		color_fprintf(stdout, PERF_COLOR_GREEN, "%-3s", status);

	printf(" ]");
}

static void status_print(const char *name, const char *macro,
			 const char *status)
{
	printf("%22s: ", name);
	on_off_print(status);
	printf("  # %s\n", macro);
}

#define STATUS(feature)                                   \
do {                                                      \
	if (feature.is_builtin)                               \
		status_print(feature.name, feature.macro, "on");  \
	else                                                  \
		status_print(feature.name, feature.macro, "OFF"); \
} while (0)

/**
 * check whether "feature" is built-in with perf
 * returns:
 *   -1: Feature not known
 *    0: Built-in
 *    1: NOT Built in
 */
static int has_support(const char *feature)
{
	int res = -1;

	for (int i = 0; supported_features[i].name; ++i) {
		if (strcmp(feature, supported_features[i].name) == 0) {
			res = supported_features[i].is_builtin;
			STATUS(supported_features[i]);
			break;
		}
	}

	if (res == -1) {
		color_fprintf(stdout, PERF_COLOR_RED, "Feature not known: %s", feature);
		return -2;
	}

	return !res;
}

int cmd_build(int argc, const char **argv)
{
	argc = parse_options(argc, argv, build_options, build_usage,
			     PARSE_OPT_STOP_AT_NON_OPTION);

	printf("perf build %s\n", perf_version_string);

	if (build.has)
		return has_support(build.has);

	return 0;
}
