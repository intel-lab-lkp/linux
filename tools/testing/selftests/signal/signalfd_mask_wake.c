// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

static int write_all(int fd, const void *buf, size_t len)
{
	const char *ptr = buf;

	while (len) {
		ssize_t ret = write(fd, ptr, len);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		ptr += ret;
		len -= ret;
	}

	return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
	char *ptr = buf;

	while (len) {
		ssize_t ret = read(fd, ptr, len);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (ret == 0)
			return -1;

		ptr += ret;
		len -= ret;
	}

	return 0;
}

static void kill_and_reap(pid_t pid)
{
	int status;

	if (pid <= 0)
		return;

	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
}

static int wait_result(int fd, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	int ret;

	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static int child_in_signalfd_read(pid_t pid, int sfd)
{
	char path[64];
	char buf[256];
	long nr;
	unsigned long arg0;
	FILE *fp;
	int in_read = 0;

	snprintf(path, sizeof(path), "/proc/%d/syscall", pid);
	fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT)
			return -1;
		return 0;
	}

	if (fgets(buf, sizeof(buf), fp) &&
	    sscanf(buf, "%ld %lx", &nr, &arg0) == 2 &&
	    nr == __NR_read && arg0 == (unsigned long)sfd)
		in_read = 1;

	fclose(fp);
	return in_read;
}

static int wait_for_child_block(pid_t pid, int sfd, int timeout_ms)
{
	int waited_ms = 0;
	int ret;

	while (waited_ms < timeout_ms) {
		ret = child_in_signalfd_read(pid, sfd);
		if (ret < 0)
			return ret;
		if (ret > 0) {
			usleep(10000);
			return 0;
		}

		usleep(1000);
		waited_ms++;
	}

	return -1;
}

int main(void)
{
	struct signalfd_siginfo fdsi;
	sigset_t blocked, initial_mask, updated_mask;
	int ready_pipe[2], start_pipe[2], result_pipe[2];
	int sfd, signo, status;
	pid_t child;
	char ready, start;

	ksft_print_header();
	ksft_set_plan(1);

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &blocked, NULL))
		ksft_exit_fail_perror("sigprocmask");

	sigemptyset(&initial_mask);
	sigaddset(&initial_mask, SIGUSR1);
	sfd = signalfd(-1, &initial_mask, 0);
	if (sfd < 0)
		ksft_exit_fail_perror("signalfd");

	if (pipe(ready_pipe))
		ksft_exit_fail_perror("pipe(ready)");
	if (pipe(start_pipe))
		ksft_exit_fail_perror("pipe(start)");
	if (pipe(result_pipe))
		ksft_exit_fail_perror("pipe(result)");

	child = fork();
	if (child < 0)
		ksft_exit_fail_perror("fork");

	if (child == 0) {
		close(ready_pipe[0]);
		close(start_pipe[1]);
		close(result_pipe[0]);

		ready = 'R';
		if (write_all(ready_pipe[1], &ready, sizeof(ready)))
			_exit(EXIT_FAILURE);

		if (read_all(start_pipe[0], &start, sizeof(start)))
			_exit(EXIT_FAILURE);

		memset(&fdsi, 0, sizeof(fdsi));
		if (read_all(sfd, &fdsi, sizeof(fdsi)))
			_exit(EXIT_FAILURE);

		signo = fdsi.ssi_signo;
		if (write_all(result_pipe[1], &signo, sizeof(signo)))
			_exit(EXIT_FAILURE);

		_exit(EXIT_SUCCESS);
	}

	close(ready_pipe[1]);
	close(start_pipe[0]);
	close(result_pipe[1]);

	if (read_all(ready_pipe[0], &ready, sizeof(ready))) {
		kill_and_reap(child);
		ksft_exit_fail_msg("child did not reach blocking read\n");
	}

	if (kill(child, SIGUSR2)) {
		kill_and_reap(child);
		ksft_exit_fail_perror("kill(SIGUSR2)");
	}

	start = 'S';
	if (write_all(start_pipe[1], &start, sizeof(start))) {
		kill_and_reap(child);
		ksft_exit_fail_msg("could not release child into signalfd read\n");
	}

	status = wait_for_child_block(child, sfd, 1000);
	if (status < 0) {
		kill_and_reap(child);
		if (status == -1)
			ksft_exit_skip("/proc/<pid>/syscall is unavailable\n");
		ksft_exit_fail_msg("child did not block in signalfd read\n");
	}

	sigemptyset(&updated_mask);
	sigaddset(&updated_mask, SIGUSR2);
	if (signalfd(sfd, &updated_mask, 0) < 0) {
		kill_and_reap(child);
		ksft_exit_fail_perror("signalfd(reconfigure)");
	}

	if (wait_result(result_pipe[0], 1000) == 1) {
		if (read_all(result_pipe[0], &signo, sizeof(signo))) {
			kill_and_reap(child);
			ksft_exit_fail_msg("child wakeup did not carry a signal\n");
		}

		if (waitpid(child, &status, 0) != child) {
			kill_and_reap(child);
			ksft_exit_fail_perror("waitpid");
		}
		ksft_test_result(signo == SIGUSR2 && WIFEXITED(status) &&
				 WEXITSTATUS(status) == 0,
				 "shared signalfd wakeup after mask update\n");
		ksft_exit_pass();
	}

	/*
	 * Buggy kernels leave the child asleep until an unrelated signal hits
	 * the child's signalfd waitqueue. SIGUSR1 does not match the updated
	 * signalfd mask, so a successful read of SIGUSR2 after this nudge
	 * demonstrates the missed wakeup.
	 */
	if (kill(child, SIGUSR1)) {
		kill_and_reap(child);
		ksft_exit_fail_perror("kill(SIGUSR1)");
	}

	if (wait_result(result_pipe[0], 1000) == 1 &&
	    !read_all(result_pipe[0], &signo, sizeof(signo)) &&
	    waitpid(child, &status, 0) == child &&
	    signo == SIGUSR2 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		ksft_test_result_fail("mask update missed a shared signalfd waiter\n");
		ksft_print_msg("child woke only after an unrelated SIGUSR1\n");
		ksft_exit_fail();
	}

	kill_and_reap(child);
	ksft_exit_fail_msg("child remained blocked after signalfd reconfiguration\n");
}
