// SPDX-License-Identifier: GPL-2.0

#include <linux/compiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../tests.h"

static int loops = 100;
static char buf;
int context_switch_loop_work = 1234;

#define write_block(fd) \
	do { \
		if (write(fd, &buf, 1) <= 0) \
			exit(1); \
	} while (0)

#define read_block(fd) \
	do { \
		if (read(fd, &buf, 1) <= 0) \
			exit(1); \
	} while (0)

/* Not static to avoid LTO clobbering the function name */
void context_switch_loop_proc1(int in_fd, int out_fd);
noinline void context_switch_loop_proc1(int in_fd, int out_fd)
{
	for (int i = 0; i < loops; i++) {
		read_block(in_fd);
		context_switch_loop_work += i * 3;
		write_block(out_fd);
	}
}

void context_switch_loop_proc2(int in_fd, int out_fd);
noinline void context_switch_loop_proc2(int in_fd, int out_fd)
{
	for (int i = 0; i < loops; i++) {
		write_block(out_fd);
		context_switch_loop_work += i * 7;
		read_block(in_fd);
	}
}

/*
 * Launches two processes that take turns to execute a multiplication N times
 */
static int context_switch_loop(int argc, const char **argv)
{
	int a_to_b[2], b_to_a[2];
	pid_t proc1_pid;
	int status;

	if (argc > 0) {
		loops = atoi(argv[0]);
		if (loops < 0) {
			fprintf(stderr, "Invalid number of loops: %s\n", argv[0]);
			return 1;
		}
	}

	if (pipe(a_to_b) || pipe(b_to_a)) {
		perror("Pipe error");
		return 1;
	}

	proc1_pid = fork();
	if (proc1_pid < 0) {
		perror("Fork error");
		return 1;
	}

	if (!proc1_pid) {
		close(a_to_b[0]);
		close(b_to_a[1]);
		prctl(PR_SET_NAME, "proc1", 0, 0, 0);
		context_switch_loop_proc1(b_to_a[0], a_to_b[1]);
		exit(0);
	}

	prctl(PR_SET_NAME, "proc2", 0, 0, 0);
	context_switch_loop_proc2(a_to_b[0], b_to_a[1]);

	if (waitpid(proc1_pid, &status, 0) != proc1_pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return 1;

	return 0;
}

DEFINE_WORKLOAD(context_switch_loop);
