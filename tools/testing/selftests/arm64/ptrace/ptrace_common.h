/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared helpers for the arm64 ptrace self-tests.
 */
#ifndef ARM64_PTRACE_COMMON_H
#define ARM64_PTRACE_COMMON_H

#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/ptrace.h>
#include <asm/ptrace.h>

/* Program counter of the tracee (0 on failure). */
static inline unsigned long tracee_pc(pid_t pid)
{
	struct iovec iov;
	struct user_pt_regs regs;

	iov.iov_base = &regs;
	iov.iov_len = sizeof(regs);
	if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) < 0)
		return 0;
	return regs.pc;
}

/* PTRACE_SYSCALL_INFO_* op of the current stop, plus syscall nr on entry. */
static inline int syscall_info(pid_t pid, long *nr)
{
	struct ptrace_syscall_info info;

	memset(&info, 0, sizeof(info));
	if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info),
		   &info) < 0)
		return PTRACE_SYSCALL_INFO_NONE;
	if (info.op == PTRACE_SYSCALL_INFO_ENTRY && nr)
		*nr = (long)info.entry.nr;
	return info.op;
}

static inline int traceme(void)
{
	return ptrace(PTRACE_TRACEME, 0, 0, 0);
}

/* Stop tracing the tracee and reap it so it does not linger as a zombie. */
static inline void tracee_kill(pid_t pid)
{
	ptrace(PTRACE_KILL, pid, 0, 0);
	waitpid(pid, NULL, 0);
}

/* Wall-clock guard so a broken kernel cannot hang the test run. */
static volatile sig_atomic_t ptrace_timed_out;

static void ptrace_timeout_handler(int sig)
{
	(void)sig;
	ptrace_timed_out = 1;
}

static inline void ptrace_timeout_start(unsigned int seconds)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = ptrace_timeout_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, NULL);
	alarm(seconds);
}

static inline void ptrace_timeout_stop(void)
{
	alarm(0);
}

#endif /* ARM64_PTRACE_COMMON_H */
