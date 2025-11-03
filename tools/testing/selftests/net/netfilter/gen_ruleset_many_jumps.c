// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>

#define MAX_LEVELS 10
#define MAX_JUMPS_PER_LEVEL 4

static void create_ruleset(int *rules, int *depth)
{
	struct timeval tv;
	int levels;
	int i;

	gettimeofday(&tv, NULL);
	srand(tv.tv_usec);

	levels = (random() % (MAX_LEVELS - 1)) + 2;
	rules[0] = 1;
	for (i = 1; i < levels; i++)
		rules[i] = (random() % MAX_JUMPS_PER_LEVEL) + 1;

	*depth = levels;

#if DEBUG_RULESET
	for (i = 0; i < depth; i++)
		printf("%u : %u\n", i, depth);
#endif
}

static void count_jumps(int *count, int *rules, int depth)
{
	int tmp[MAX_LEVELS] = {};
	int i = 0;

	while (1) {
		if (tmp[i]++ < rules[i]) {
			(*count)++;
			if (i < depth - 1)
				i++;
		} else {
			tmp[i] = 0;
			if (--i <= 0)
				break;
		}
	}
}

static int print_ruleset(int *rules, int depth, int jump_count, char *filename)
{
	int fd, i, j;
	FILE *fp;

	fd = mkstemp(filename);
	if (fd < 0) {
		fprintf(stderr, "failed to create temporary ruleset file: %s\n", strerror(errno));
		return -1;
	}

	fp = fdopen(fd, "w+");
	if (!fp) {
		close(fd);
		fprintf(stderr, "failed to create temporary ruleset file\n");
		return -1;
	}

	fprintf(fp, "# jump_count %d\n", jump_count);
	fprintf(fp, "table ip x {\n");
	fprintf(fp, "\tchain y%u {\n", depth);
	fprintf(fp, "\t}\n");

	for (i = depth - 1; i >= 1; i--) {
		fprintf(fp, "\tchain y%u {\n", i);
		for (j = 0; j < rules[i]; j++)
			fprintf(fp, "\t\tjump y%d\n", i+1);

		fprintf(fp, "\t}\n");
	}
	fprintf(fp, "\tchain y0 {\n", i);
	fprintf(fp, "\t\ttype filter hook input priority 0;\n");
	fprintf(fp, "\t\tjump y1\n");
	fprintf(fp, "\t}\n");
	fprintf(fp, "}\n");

	return 0;
}

enum {
	RANDOM = 0,
	FAIL,
	OK,
};

int main(int argc, const char *argv[])
{
	unsigned int type, nf_tables_jumps_max_netns;
	int rules[10], depth, i, jump_count = 0;
	char filename[] = "/tmp/rulesetXXXXXX";

	if (argc == 3) {
		if (!strcmp(argv[1], "ok"))
			type = OK;
		else if (!strcmp(argv[1], "fail"))
			type = FAIL;

		nf_tables_jumps_max_netns = atoi(argv[2]);
	} else {
		type = RANDOM;
	}

	switch (type) {
	case RANDOM:
		memset(rules, 0, sizeof(rules));
		create_ruleset(rules, &depth);
		count_jumps(&jump_count, rules, depth);
		break;
	case OK:
		while (1) {
			memset(rules, 0, sizeof(rules));
			create_ruleset(rules, &depth);
			count_jumps(&jump_count, rules, depth);
			if (jump_count <= nf_tables_jumps_max_netns)
				break;

			jump_count = 0;
		}
		break;
	case FAIL:
		while (1) {
			memset(rules, 0, sizeof(rules));
			create_ruleset(rules, &depth);
			count_jumps(&jump_count, rules, depth);
			if (jump_count > nf_tables_jumps_max_netns)
				break;

			jump_count = 0;
		}
		break;
	}
	print_ruleset(rules, depth, jump_count, filename);
	printf("%s\n", filename);
}
