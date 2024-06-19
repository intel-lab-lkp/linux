// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <linux/elf.h>
#include <linux/unistd.h>
#include <asm/ptrace.h>

#include "../../kselftest_harness.h"

#define ORIG_A0_AFTER_MODIFIED  0x5
#define MODIFY_A0               0x01
#define MODIFY_ORIG_A0          0x02

#define perr_and_exit(fmt, ...) do {			\
	char buf[256];					\
	snprintf(buf, sizeof(buf), "%s:%d: " fmt ": %m\n",	\
			__func__, __LINE__, ##__VA_ARGS__);	\
	perror(buf);						\
	exit(-1);						\
} while (0)

static inline void resume_and_wait_tracee(pid_t pid, int flag)
{
	int status;

	if (ptrace(flag, pid, 0, 0))
		perr_and_exit("failed to resume the tracee %d", pid);

	if (waitpid(pid, &status, 0) != pid)
		perr_and_exit("failed to wait for the tracee %d", pid);
}

static void ptrace_restart_syscall(int opt, int *result)
{
	int status;
	int p[2], fd_zero;
	pid_t pid;

	struct user_regs_struct regs;
	struct iovec iov = {
		.iov_base = &regs,
		.iov_len = sizeof(regs),
	};

	if (pipe(p))
		perr_and_exit("failed to create a pipe");

	fd_zero = open("/dev/zero", O_RDONLY);
	if (fd_zero < 0)
		perr_and_exit("failed to open /dev/zero");

	pid = fork();
	if (pid == 0) {
		char c;

		/* Mark oneself being traced */
		if (ptrace(PTRACE_TRACEME, 0, 0, 0))
			perr_and_exit("failed to request for tracer to trace me");

		kill(getpid(), SIGSTOP);

		if (read(p[0], &c, 1) != 1)
			exit(1);

		exit(0);
	} else if (pid < 0)
		exit(1);

	if (waitpid(pid, &status, 0) != pid)
		perr_and_exit("failed to wait for the tracee %d\n", pid);

	/* Resume the tracee until the next syscall */
	resume_and_wait_tracee(pid, PTRACE_SYSCALL);

	/* Deliver a signal to interrupt the syscall */
	kill(pid, SIGUSR1);

	/* The tracee stops at syscall exit */
	resume_and_wait_tracee(pid, PTRACE_SYSCALL);

	/* Check tracee orig_a0 before syscall restart */
	if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov))
		perr_and_exit("failed to get tracee registers");
	if (regs.orig_a0 != p[0])
		perr_and_exit("unexpected a0");

	/* Modify a0/orig_a0 for the restarted syscall */
	switch (opt) {
	case MODIFY_A0:
		regs.a0 = fd_zero;
		break;
	case MODIFY_ORIG_A0:
		regs.orig_a0 = fd_zero;
		break;
	}

	if (ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov))
		perr_and_exit("failed to set tracee registers");

	/* Ignore SIGUSR1 signal */
	resume_and_wait_tracee(pid, PTRACE_SYSCALL);

	/* Stop at the entry point of the restarted syscall */
	resume_and_wait_tracee(pid, PTRACE_SYSCALL);

	/* Now, check regs.a0 of the restarted syscall */
	if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov))
		perr_and_exit("failed to get tracee registers");
	*result = regs.a0;

	/* Resume the tracee */
	ptrace(PTRACE_CONT, pid, 0, 0);
	if (waitpid(pid, &status, 0) != pid)
		perr_and_exit("failed to wait for the tracee");
}

TEST(ptrace_modify_a0)
{
	int result;

	ptrace_restart_syscall(MODIFY_A0, &result);

	/* The tracer's modification of a0 cannot affect the restarted tracee */
	EXPECT_NE(ORIG_A0_AFTER_MODIFIED, result);
}

TEST(ptrace_modify_orig_a0)
{
	int result;

	ptrace_restart_syscall(MODIFY_ORIG_A0, &result);

	/* The tracer must modify orig_a0 to actually change the tracee's a0 */
	EXPECT_EQ(ORIG_A0_AFTER_MODIFIED, result);
}

TEST_HARNESS_MAIN
