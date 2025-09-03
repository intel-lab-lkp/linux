// SPDX-License-Identifier: GPL-2.0
/*
 * Seccomp filter example for cloning a filter
 *
 * Copyright (c) 2025 Oracle and/or its affiliates.
 * Author: Tom Hromatka <tom.hromatka@oracle.com>
 *
 * The code may be used by anyone for any purpose,
 * and can serve as a starting point for developing
 * applications that reuse the same seccomp filter
 * across many processes.
 */
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

static int seccomp(unsigned int op, unsigned int flags, void *args)
{
	errno = 0;
	return syscall(__NR_seccomp, op, flags, args);
}

static int install_filter(void)
{
	struct sock_filter deny_filter[] = {
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
			offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_getppid, 0, 1),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | ESRCH),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog deny_prog = {
		.len = (unsigned short)ARRAY_SIZE(deny_filter),
		.filter = deny_filter,
	};

	return seccomp(SECCOMP_SET_MODE_FILTER, 0, &deny_prog);
}

static int clone_filter(pid_t ref_pid)
{
	int ref_pidfd, ret;

	ref_pidfd = syscall(SYS_pidfd_open, ref_pid, 0);
	if (ref_pidfd < 0)
		return -errno;

	ret = seccomp(SECCOMP_CLONE_FILTER, 0, &ref_pidfd);

	close(ref_pidfd);

	return ret;
}

static void do_ref_filter(void)
{
	int ret;

	ret = install_filter();
	if (ret) {
		perror("Failed to install ref filter\n");
		exit(1);
	}

	while (true)
		sleep(1);
}

static void do_child_process(pid_t ref_pid)
{
	pid_t res;
	int ret;

	ret = clone_filter(ref_pid);
	if (ret != 0) {
		perror("Failed to clone filter. Installing filter from scratch\n");

		ret = install_filter();
		if (ret != 0) {
			perror("Filter install failed\n");
			exit(ret);
		}
	}

	res = syscall(__NR_getpid);
	if (res < 0) {
		perror("getpid() unexpectedly failed\n");
		exit(errno);
	}

	res = syscall(__NR_getppid);
	if (res > 0) {
		perror("getppid() unexpectedly succeeded\n");
		exit(1);
	}

	exit(0);
}

int main(void)
{
	pid_t ref_pid = -1, child_pid = -1;
	int ret, status;

	ref_pid = fork();
	if (ref_pid < 0)
		exit(errno);
	else if (ref_pid == 0)
		do_ref_filter();

	child_pid = fork();
	if (child_pid < 0)
		goto out;
	else if (child_pid == 0)
		do_child_process(ref_pid);

	waitpid(child_pid, &status, 0);
	if (WEXITSTATUS(status) != 0) {
		perror("child process failed");
		ret = WEXITSTATUS(status);
		goto out;
	}

	ret = 0;

out:
	if (ref_pid != -1)
		kill(ref_pid, SIGKILL);
	if (child_pid != -1)
		kill(child_pid, SIGKILL);

	exit(ret);
}
