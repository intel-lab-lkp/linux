// SPDX-License-Identifier: GPL-2.0

#include <linux/compiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int loops = 100;
static char buf;
int work = 1234;

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

static noinline void thread1(int in_fd, int out_fd)
{
	for (int i = 0; i < loops; i++) {
		read_block(in_fd);
		work += i * 3;
		write_block(out_fd);
	}
}

static noinline void thread2(int in_fd, int out_fd)
{
	for (int i = 0; i < loops; i++) {
		write_block(out_fd);
		work += i * 7;
		read_block(in_fd);
	}
}

int main(int argc, char **argv)
{
	int a_to_b[2], b_to_a[2];
	pid_t thread1_pid;
	int status;

	if (argc > 1) {
		loops = atoi(argv[1]);
		if (loops < 0) {
			fprintf(stderr, "Invalid number of loops: %s\n", argv[1]);
			return 1;
		}
	}

	if (pipe(a_to_b) || pipe(b_to_a)) {
		perror("Pipe error");
		return 1;
	}

	thread1_pid = fork();
	if (thread1_pid < 0) {
		perror("Fork error");
		return 1;
	}

	if (!thread1_pid) {
		prctl(PR_SET_NAME, "thread1", 0, 0, 0);
		thread1(b_to_a[0], a_to_b[1]);
		exit(0);
	}

	prctl(PR_SET_NAME, "thread2", 0, 0, 0);
	thread2(a_to_b[0], b_to_a[1]);

	if (waitpid(thread1_pid, &status, 0) != thread1_pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		return 1;

	return 0;
}
