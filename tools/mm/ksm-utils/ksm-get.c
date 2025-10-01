// SPDX-License-Identifier: GPL-2.0-only
/*
 * ksm-get: Tool for acquire KSM-merging metrics for processes.
 *
 * Copyright (C) 2025 ZTE corporation
 *
 * Authors: xu xin <xu.xin16@zte.com.cn>
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/prctl.h>
#include <ctype.h>
#include <getopt.h>

#define INVALID_PID -1
#define MAX_FILE_NAME_SIZE 64
#define COMM_MAX_SIZE 16
#define MAX_PROCESSES 65536

/* Enum option for sorting*/
typedef enum {
	SORT_MERGING_PAGES,    /* Default, by ksm_merging_pages*/
	SORT_PROFIT
} sort_field_t;

typedef struct {
	int pid;
	char comm[COMM_MAX_SIZE];
	long ksm_merging_pages;
	long ksm_zero_pages;
	long ksm_profit;
	long ksm_rmap_items;
	int KSM_mergeable;
	int KSM_merge_any;
	int valid; /* indicate if the data is valid */
} ksm_info_t;

int pid = INVALID_PID;
int lookup_all_pid;
int need_extend_info;
sort_field_t sort_field = SORT_MERGING_PAGES;

static void usage(void)
{
	fprintf(stderr, "Usage: ksm-show [-a] [-p pid] [--sort field] [-e]\n\n");
	printf("Show KSM merging information of processes.\n\n"
		"Get KSM merging information of a specific process:\n"
		" ksm-show -p pid\n\n"
		"Get KSM merging information of all processes:\n"
		" ksm-show -a\n\n"
		"Options:\n"
		"-a, --all              show all processes (default sort by merging_pages)\n"
		"-p, --pid [pid]        show specific process\n"
		"--sort [field]         sort field: merging_pages or profit\n"
		"-e                     display extended information\n"
		"-h, --help             show this help\n\n"
		"Default columns: Pid, Comm, Merging_pages, Ksm_zero_pages, Ksm_profit\n"
	);
}

static inline bool pid_is_set(void)
{
	return (pid != INVALID_PID);
}

static int check_arguments(void)
{
	if (pid_is_set() && lookup_all_pid) {
		fprintf(stderr, "error: Options -a and -p cannot be used together.\n");
		return -EINVAL;
	}

	if (!pid_is_set() && !lookup_all_pid) {
		fprintf(stderr, "error: Either -a or -p must be specified.\n");
		return -EINVAL;
	}

	return 0;
}

static void print_header(void)
{
	printf("%-12s", "Pid");
	printf("%-20s", "Comm");
	printf("%-15s", "Merging_pages");
	printf("%-18s", "Ksm_zero_pages");
	printf("%-15s", "Ksm_profit");
	if (need_extend_info) {
		printf("%-18s", "Ksm_mergeable");
		printf("%-18s", "Ksm_merge_any");
	}
	printf("\n");
}

static long parse_proc_ksm_stat(char *buf, char *field)
{
	char *substr;
	size_t value_pos;

	substr = strstr(buf, field);
	if (!substr)
		return -1;

	if (!strcmp(field, "ksm_mergeable") ||
	    !strcmp(field, "ksm_merge_any")) {
		if (!strncmp(substr + strlen(field) + 2, "yes", 3))
			return 1;
		else
			return 0;
	}

	value_pos = strcspn(substr, "0123456789");
	return strtol(substr + value_pos, NULL, 10);
}

static void get_pid_comm(int this_pid, char *comm, int len)
{
	int comm_fd, read_size;
	char proc_comm_name[MAX_FILE_NAME_SIZE];

	snprintf(proc_comm_name, MAX_FILE_NAME_SIZE, "/proc/%d/comm", this_pid);
	comm_fd = open(proc_comm_name, O_RDONLY);
	if (comm_fd < 0)
		return;

	read_size = pread(comm_fd, comm, len - 1, 0);
	close(comm_fd);

	if (read_size <= 0)
		return;

	/* make sure string end with \0 */
	if (comm[read_size - 1] == '\n')
		comm[read_size - 1] = '\0';
	else if (read_size < len - 1)
		comm[read_size] = '\0';
	else
		comm[len - 1] = '\0';
}

static int get_pid_ksm_info(int this_pid, ksm_info_t *info)
{
	int proc_fd, read_size;
	char proc_name[MAX_FILE_NAME_SIZE];
	char buf[256];

	memset(info, 0, sizeof(ksm_info_t));
	info->pid = this_pid;
	info->valid = 0;

	get_pid_comm(this_pid, info->comm, COMM_MAX_SIZE);
	snprintf(proc_name, MAX_FILE_NAME_SIZE, "/proc/%d/ksm_stat", this_pid);

	proc_fd = open(proc_name, O_RDONLY);
	/* ksm_stat doesn't exist, maybe kthread or CONFIG_KSM disabled. */
	if (proc_fd < 0)
		return -1;

	read_size = pread(proc_fd, buf, sizeof(buf) - 1, 0);
	close(proc_fd);

	if (read_size <= 0)
		return -1;


	buf[read_size] = 0;

	info->ksm_merging_pages = parse_proc_ksm_stat(buf, "ksm_merging_pages");
	info->ksm_zero_pages = parse_proc_ksm_stat(buf, "ksm_zero_pages");
	info->ksm_profit = parse_proc_ksm_stat(buf, "ksm_process_profit");
	info->ksm_rmap_items = parse_proc_ksm_stat(buf, "ksm_rmap_items");
	info->KSM_mergeable = parse_proc_ksm_stat(buf, "ksm_mergeable");
	info->KSM_merge_any = parse_proc_ksm_stat(buf, "ksm_merge_any");

	if (info->ksm_merging_pages < 0 || info->ksm_profit < 0)
		return -1;

	info->valid = 1;
	return 0;
}

static void print_ksm_info(ksm_info_t *info)
{
	if (!info->valid) {
		printf("%-12d", info->pid);
		printf("%-20s", info->comm);
		printf("%-15s", "N/A");
		printf("%-18s", "N/A");
		printf("%-15s", "N/A");
		printf("\n");
		return;
	}

	printf("%-12d", info->pid);
	printf("%-20s", info->comm);
	printf("%-15ld", info->ksm_merging_pages);
	printf("%-18ld", info->ksm_zero_pages);
	printf("%-15ld", info->ksm_profit);
	if (need_extend_info) {
		printf("%-18s", info->KSM_mergeable >= 0 ?
			(info->KSM_mergeable ? "yes" : "no") : "N/A");
		printf("%-18s", info->KSM_merge_any >= 0 ?
			(info->KSM_merge_any ? "yes" : "no") : "N/A");
	}
	printf("\n");
}

/* sort by ksm_merging_pages in descending order */
static int compare_by_merging_pages(const void *a, const void *b)
{
	const ksm_info_t *info_a = (const ksm_info_t *)a;
	const ksm_info_t *info_b = (const ksm_info_t *)b;

	/* The valid data is put at first */
	if (info_a->valid && !info_b->valid)
		return -1;
	if (!info_a->valid && info_b->valid)
		return 1;
	if (!info_a->valid && !info_b->valid)
		return 0;

	/*  list in descending order */
	if (info_a->ksm_merging_pages > info_b->ksm_merging_pages)
		return -1;
	if (info_a->ksm_merging_pages < info_b->ksm_merging_pages)
		return 1;

	return 0;
}

/* sort by ksm_profit in descending order */
static int compare_by_profit(const void *a, const void *b)
{
	const ksm_info_t *info_a = (const ksm_info_t *)a;
	const ksm_info_t *info_b = (const ksm_info_t *)b;

	/* The valid data is put at first */
	if (info_a->valid && !info_b->valid)
		return -1;
	if (!info_a->valid && info_b->valid)
		return 1;
	if (!info_a->valid && !info_b->valid)
		return 0;

	/*  list in descending order */
	if (info_a->ksm_profit > info_b->ksm_profit)
		return -1;
	if (info_a->ksm_profit < info_b->ksm_profit)
		return 1;

	return 0;
}

static int collect_all_ksm_info(ksm_info_t *infos, int max_infos)
{
	DIR *dir;
	struct dirent *entry;
	int this_pid;
	int count = 0;

	dir = opendir("/proc");
	if (!dir) {
		perror("cannot open /proc");
		return -1;
	}

	while ((entry = readdir(dir)) != NULL && count < max_infos) {
		/* Check if the dir name is digital (process dir) */
		if (isdigit(entry->d_name[0]))
			if (sscanf(entry->d_name, "%d", &this_pid) == 1)
				if (get_pid_ksm_info(this_pid, &infos[count]) == 0)
					count++;
	}

	closedir(dir);
	return count;
}

static void show_sorted_ksm_stat(void)
{
	ksm_info_t *infos;
	int count, i;

	infos = malloc(MAX_PROCESSES * sizeof(ksm_info_t));
	if (!infos) {
		perror("malloc failed");
		return;
	}

	count = collect_all_ksm_info(infos, MAX_PROCESSES);
	if (count < 0) {
		free(infos);
		return;
	}

	/* pick the sort function by sort filed */
	if (sort_field == SORT_MERGING_PAGES)
		qsort(infos, count, sizeof(ksm_info_t), compare_by_merging_pages);
	else if (sort_field == SORT_PROFIT)
		qsort(infos, count, sizeof(ksm_info_t), compare_by_profit);

	for (i = 0; i < count; i++)
		print_ksm_info(&infos[i]);

	free(infos);
}

static void show_single_ksm_stat(void)
{
	ksm_info_t info;

	if (get_pid_ksm_info(pid, &info) == 0)
		print_ksm_info(&info);
	else
		fprintf(stderr, "Error: Cannot get KSM info for pid %d\n", pid);
}

int main(int argc, char **argv)
{
	int err;
	int opt;
	int option_index = 0;

	// Define long-option
	static struct option long_options[] = {
		{"all", no_argument, 0, 'a'},
		{"pid", required_argument, 0, 'p'},
		{"sort", required_argument, 0, 's'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	if (argc == 1) {
		usage();
		return 1;
	}

	/* Parse the arguments */
	while ((opt = getopt_long(argc, argv, "ap:s:eh", long_options, &option_index)) != -1) {
		switch (opt) {
		case 'a':
			lookup_all_pid = 1;
			break;
		case 'p':
			if (sscanf(optarg, "%d", &pid) != 1) {
				fprintf(stderr, "Invalid argument for -p: %s\n", optarg);
				return 1;
			}
			break;
		case 's':  // sort option
			if (strcmp(optarg, "merging_pages") == 0) {
				sort_field = SORT_MERGING_PAGES;
			} else if (strcmp(optarg, "profit") == 0) {
				sort_field = SORT_PROFIT;
			} else {
				fprintf(stderr, "Error sort field: %s. Use merging_pages or profit\n",
					optarg);
				return 1;
			}
			break;
		case 'e':
			need_extend_info = 1;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 1;
		}
	}

	/* Chech if there is unknown argument.*/
	if (optind < argc) {
		fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
		usage();
		return 1;
	}

	err = check_arguments();
	if (err < 0)
		return -EINVAL;

	print_header();
	if (lookup_all_pid)
		show_sorted_ksm_stat();
	else
		show_single_ksm_stat();

	return 0;
}
