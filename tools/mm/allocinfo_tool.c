// SPDX-License-Identifier: GPL-2.0
/*
 * allocinfo_tool: Tool to parse allocinfo
 *
 * Authors: Hao Ge <hao.ge@linux.dev>
 *
 * Compile with:
 * gcc -o allocinfo_tool allocinfo_tool.c
 */

#include<stdio.h>
#include<stdlib.h>
#include<sys/stat.h>
#include <string.h>
#include <getopt.h>

#define ALLOCINFO_FILE "/proc/allocinfo"
#define BUF_SIZE 1024
#define NAME_MAX_LENTH 64

struct alloc_tag_counters {
	signed long long bytes;
	unsigned long long calls;
};

struct codetag {
	unsigned int lineno;
	char modname[NAME_MAX_LENTH];
	char function[NAME_MAX_LENTH];
	char filename[NAME_MAX_LENTH];
};

struct alloc_info {
	struct alloc_tag_counters counters;
	struct codetag tag;
	int has_modname;
};

static int arg_opt;

enum OPT_BIT {
	SKIP_ZERO = 1 << 0,
};

void usage(void)
{
	printf("Usage: ./allocinfo_tool [OPTIONS]\n"
	       "-s\t\t\tskip bytes for 0 allocinfo entries\n"
	);
}

int parse_alloc_info(char *line, struct alloc_info *info)
{
	if (sscanf(line, "%12lli %8llu %[^:]:%d [%[^]]] func:%s",
		      &info->counters.bytes, &info->counters.calls,
		      info->tag.filename, &info->tag.lineno,
		      info->tag.modname, info->tag.function) == 6){
		info->has_modname = 1;
		return 1;
	};

	if (sscanf(line, "%12llu %8llu %[^:]:%u func:%s",
		   &info->counters.bytes, &info->counters.calls,
		   info->tag.filename, &info->tag.lineno,
		   info->tag.function) == 5){
		info->has_modname = 0;
		return 1;
	}

	return 0;
}

int read_alloc_info(void)
{
	FILE *file = fopen(ALLOCINFO_FILE, "r");

	if (!file) {
		perror("Failed to open /proc/allocinfo");
		return EXIT_FAILURE;
	}

	int line = 0, i = 0;
	char *buffer = malloc(BUF_SIZE);
	struct alloc_info *info;

	while (fgets(buffer, BUF_SIZE, file)) {

		/*
		 * allocinfo - version: 1.0
		 * #     <size>  <calls> <tag info>
		 */
		if (line < 2) {
			printf("%s", buffer);
			line++;
			continue;
		}

		info = realloc(info, sizeof(struct alloc_info) * (i + 1));

		if (parse_alloc_info(buffer, info + i) == 0) {
			printf("Mismatch with the format of /proc/allocinfo");
			return 0;
		}

		if ((arg_opt & SKIP_ZERO) && (info[i].counters.bytes == 0))
			continue;

		printf("%12lli %8llu ", info[i].counters.bytes, info[i].counters.calls);

		if (info[i].has_modname)
			printf("%s:%u [%s] func:%s",
			       info[i].tag.filename, info[i].tag.lineno,
			       info[i].tag.modname, info[i].tag.function);
		else
			printf("%s:%u func:%s",
			       info[i].tag.filename, info[i].tag.lineno,
			       info[i].tag.function);
		printf(" ");
		printf("\n");
		i++;
	}

	free(info);
	free(buffer);
	fclose(file);
}

int main(int argc, char *argv[])
{

	int opt;
	struct option longopts[] = {
		{ "s", 0, NULL, 1},
		{ 0, 0, 0, 0},
	};

	while ((opt = getopt_long(argc, argv, "s", longopts, NULL)) != -1) {
		switch (opt) {
		case 's':
			arg_opt = arg_opt | SKIP_ZERO;
			break;
		default:
			usage();
			exit(1);
		}
	}

	read_alloc_info();

}
