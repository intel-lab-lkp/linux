// SPDX-License-Identifier: GPL-2.0-only
/*
 * ksm-set: Tool for enabling/disabling KSM-merging for a process.
 *
 * Copyright (C) 2024 ZTE corporation
 *
 * Authors: xu xin <xu.xin16@zte.com.cn>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/prctl.h>
#include <stdbool.h>

#include <linux/prctl.h>

#define INVALID_PID -1
#define KSM_ENABLE_UNSET -1

char **command;
int ksm_enable = KSM_ENABLE_UNSET;
int pid = INVALID_PID;

static inline bool command_is_set(void)
{
	return (!!command);
}

static inline bool ksm_enable_is_set(void)
{
	return (ksm_enable != KSM_ENABLE_UNSET);
}

static void usage(void)
{
	fprintf(stderr, "Usage: ksm-set -s [on|off] [<command> [<arg>...]]\n\n");
	printf("Change the KSM merging attributes of processes.\n\n"
	   "Enable/disable KSM merging any anonymous VMA when starting a new process:\n"
	   " ksm-set -s [on|off] <command> [<arg>...]\n\n"
	   "Options:\n"
	   "-s [on|off]    enable or disable KSM merging\n"
	   "-h,--help      show this help\n\n"
	);
}

static int check_arguments(void)
{
	if (!ksm_enable_is_set()) {
		fprintf(stderr, "error: Option -s is required.\n");
		return -EINVAL;
	}

	if (!command_is_set()) {
		fprintf(stderr, "error: Command must be specified.\n");
		return -EINVAL;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int index, nr_cmd_args, err;
	char *buffer = NULL;

	if (argc == 1) {
		usage();
		return 1;
	}

	/* Parsing the argument*/
	for (index = 1; index < argc; index++) {
		if (argv[index][0] == '-') {
			switch (argv[index][1]) {
			case 'p':
				if (index >= argc - 1) {
					fprintf(stderr, "Invalid argument for -p\n");
					return 1;
				}
				if (sscanf(argv[index + 1], "%d", &pid) != 1) {
					fprintf(stderr, "Invalid argument for -p\n");
					return 1;
				}
				index++;
				break;
			case 's':
				if (index >= argc - 1) {
					fprintf(stderr, "Invalid argument for -s\n");
					return -EINVAL;
				}
				buffer = argv[index + 1];
				if (strcmp(buffer, "on") == 0)
					ksm_enable = 1;
				else if (strcmp(buffer, "off") == 0)
					ksm_enable = 0;
				else {
					fprintf(stderr, "Invalid argument for-s: must be 'on' or 'off'\n");
					return -EINVAL;
				}
				index++;
				break;
			case 'h':
				usage();
				return 0;
			default:
				fprintf(stderr, "Unknown option: %s\n", argv[index]);
				usage();
				return 1;
			}
		} else {
			/*
			 * The remained arguments is seen as a command
			 * with arguments.
			 */
			command = argv + index;
			nr_cmd_args = argc - index;
			break;
		}
	}

	err = check_arguments();
	if (err < 0)
		return -EINVAL;

	printf("KSM %s: ", ksm_enable ? "enabled" : "disabled");
	for (index = 0; index < nr_cmd_args; index++)
		printf("%s ", command[index]);
	printf("\n");

	err = prctl(PR_SET_MEMORY_MERGE, ksm_enable, 0, 0, 0);
	if (err != 0) {
		perror("prctl PR_SET_MEMORY_MERGE failed");
		return -errno;
	}

	execvp(command[0], command);
	perror("execvp failed");
	return -errno;

	return 0;
}
