// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <libfdt.h>

#include "util.h"

static const char usage_synopsis[] =
	"find the best matching device tree from a reference board\n"
	"	fdt-select-board <options> [<candidate.dtb>]\n"
	"\n";
static const char usage_short_opts[] = "r:av" USAGE_COMMON_SHORT_OPTS;
static const struct option usage_long_opts[] = {
	{"reference", required_argument, NULL, 'r'},
	{"verbose",	    no_argument, NULL, 'v'},
	{"all",		    no_argument, NULL, 'a'},
	USAGE_COMMON_LONG_OPTS
};

static const char * const usage_opts_help[] = {
	"Reference DTB",
	"Verbose messages",
	"List all matches",
	USAGE_COMMON_OPTS_HELP
};

int verbose = 0;

struct context {
	const void *fdt;
	int node;
};

static const void *get_board_id(void *ctx, const char *name, int *datalen)
{
	struct context *c = ctx;

	return fdt_getprop(c->fdt, c->node, name, datalen);
}

int main(int argc, char *argv[])
{
	int opt;
	char *input_filename = NULL;
	const void *fdt;
	const void *ref;
	int ref_node;
	int max_score = -1;
	const char *best_match = NULL;
	struct context ctx;
	int all = 0;

	while ((opt = util_getopt_long()) != EOF) {
		switch (opt) {
		case_USAGE_COMMON_FLAGS

		case 'a':
			all = 1;
			break;
		case 'r':
			input_filename = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		}
	}

	if (!input_filename)
		usage("missing reference file");

	argv += optind;
	argc -= optind;

	if (argc <= 0)
		usage("missing candidate dtbs");

	ref = utilfdt_read(input_filename, NULL);
	if (!ref) {
		fprintf(stderr, "failed to read reference %s\n", input_filename);
		return -1;
	}

	ref_node = fdt_path_offset(ref, "/board-id");
	if (ref_node < 0) {
		fprintf(stderr, "reference blob doesn't have a board-id\n");
		return -1;
	}

	ctx.fdt = ref;
	ctx.node = ref_node;

	for (; argc > 0; --argc, ++argv) {
		int score;

		fdt = utilfdt_read(*argv, NULL);
		if (!fdt) {
			fprintf(stderr, "failed to read %s\n", *argv);
			return -1;
		}

		score = fdt_board_id_score(fdt, get_board_id, &ctx);
		if (verbose || (score > 0 && all))
			printf("%s: %d\n", *argv, score);

		if (score > max_score) {
			max_score = score;
			best_match = *argv;
		}

		free(fdt);
	}

	if (best_match && !all)
		printf("%s\n", best_match);

	return 0;
}
