// SPDX-License-Identifier: GPL-2.0
/*
 * Write in a pipe and wait.
 *
 * Used by layout1.umount_sandboxer from fs_test.c and
 * audit_rule.exe_landlock_deny from audit_test.c
 *
 * Copyright © 2024-2025 Microsoft Corporation
 */

#define _GNU_SOURCE
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "wrappers.h"

int main(int argc, char *argv[])
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.scoped = LANDLOCK_SCOPE_SIGNAL,
	};
	int pipe_child, pipe_parent;
	char buf;
	int ruleset_fd;

	/* The first argument must be the file descriptor number of a pipe. */
	if (argc != 3) {
		fprintf(stderr, "Wrong number of arguments (not two)\n");
		return 1;
	}

	pipe_child = atoi(argv[1]);
	pipe_parent = atoi(argv[2]);

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	if (ruleset_fd < 0) {
		perror("Failed to create a ruleset");
		return 1;
	}

	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (landlock_restrict_self(ruleset_fd, 0)) {
		perror("Failed to restrict self");
		return 1;
	}
	close(ruleset_fd);

	/* Signals that we are waiting. */
	if (write(pipe_child, ".", 1) != 1) {
		perror("Failed to write to first argument");
		return 1;
	}

	/* Waits for the parent do its test. */
	if (read(pipe_parent, &buf, 1) != 1) {
		perror("Failed to write to the second argument");
		return 1;
	}

	/* Tries to send a signal. */
	kill(getppid(), 0);

	return 0;
}
