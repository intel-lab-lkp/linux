// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that after a SECCOMP_RET_TRACE stop, changes to x0 are visible
 * to the syscall tracepoint (orig_x0 re-sync).
 *
 * The child installs a seccomp filter that returns SECCOMP_RET_TRACE for
 * write().  The parent waits for PTRACE_EVENT_SECCOMP, changes the first
 * argument (fd) from 2 to 1, then resumes the child.  By monitoring the
 * sys_enter_write tracepoint, we check whether the kernel recorded fd=1
 * (test passes) or fd=2 (test fails).
 *
 * Requires root and tracefs at /sys/kernel/debug/tracing.  If unavailable,
 * the test is skipped.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <linux/elf.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/ptrace.h>

#include "kselftest.h"

#ifndef __NR_write
#define __NR_write 64
#endif

#define PTRACE_EVENT_MASK(status) ((status) >> 16)

#define TRACEFS_PATH "/sys/kernel/debug/tracing"
#define TRACE_EVENT  "syscalls/sys_enter_write"   /* note '/' not ':' */
#define TRACE_PIPE   TRACEFS_PATH "/trace_pipe"
#define TRACE_ON     TRACEFS_PATH "/events/" TRACE_EVENT "/enable"
#define TRACE_CLR    TRACEFS_PATH "/trace"

static int do_child(void)
{
	int null_fd = open("/dev/null", O_WRONLY);

	if (null_fd >= 0) {
		dup2(null_fd, STDOUT_FILENO);
		close(null_fd);
	}

	if (ptrace(PTRACE_TRACEME, 0, NULL, NULL))
		_exit(1);
	raise(SIGSTOP);	/* let parent configure ptrace options */

	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),			/* syscall nr */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_write, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),		/* write -> trace */
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = ARRAY_SIZE(filter),
		.filter = filter,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		_exit(2);

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog))
		_exit(3);

	/* write(2, "", 0) – parent will change fd to 1 at SECCOMP stop */
	syscall(__NR_write, 2, "", 0);
	_exit(0);
}

static int trace_pipe_expect(const char *expect, int timeout_secs)
{
	FILE *fp = fopen(TRACE_PIPE, "r");
	char buf[4096];
	int found = 0;
	time_t deadline = time(NULL) + timeout_secs;

	if (!fp)
		return 0;

	fcntl(fileno(fp), F_SETFL, O_NONBLOCK);
	while (time(NULL) < deadline) {
		ssize_t n = fread(buf, 1, sizeof(buf) - 1, fp);

		if (n <= 0) {
			usleep(10000);
			continue;
		}
		buf[n] = '\0';
		if (strstr(buf, expect)) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	return found;
}

static void enable_trace(bool on)
{
	int fd = open(TRACE_ON, O_WRONLY);
	char c = on ? '1' : '0';

	if (fd >= 0) {
		write(fd, &c, 1);
		close(fd);
	}
}

static void clear_trace(void)
{
	int fd = open(TRACE_CLR, O_WRONLY);

	if (fd >= 0) {
		write(fd, "0", 1);
		close(fd);
	}
}

int main(void)
{
	/* Wait for SECCOMP event or child exit */
	bool seccomp_event = false;
	bool ok = false;
	pid_t child;
	int status;

	ksft_print_header();
	ksft_set_plan(1);

	if (geteuid() != 0) {
		ksft_test_result_skip("not root\n");
		goto out;
	}
	if (access(TRACE_ON, W_OK) != 0) {
		ksft_test_result_skip("tracefs unavailable\n");
		goto out;
	}

	child = fork();
	if (!child)
		return do_child();

	/* Wait for initial SIGSTOP */
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid initial");
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
		ksft_exit_fail_msg("unexpected initial stop");

	if (ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESECCOMP)) {
		ksft_test_result_fail("setoptions\n");
		goto out;
	}

	if (ptrace(PTRACE_CONT, child, 0, 0)) {
		ksft_test_result_fail("PTRACE_CONT\n");
		goto out;
	}

	while (1) {
		if (waitpid(child, &status, 0) != child)
			ksft_exit_fail_msg("waitpid lost");

		if (WIFEXITED(status)) {
			int code = WEXITSTATUS(status);

			ksft_test_result(code == 2 || code == 3,
					 "seccomp filter not installed\n");
			goto out;
		}
		if (WIFSIGNALED(status)) {
			ksft_test_result_fail("killed\n");
			goto out;
		}
		if (WIFSTOPPED(status)) {
			if (WSTOPSIG(status) == SIGTRAP &&
			    PTRACE_EVENT_MASK(status) == PTRACE_EVENT_SECCOMP) {
				seccomp_event = true;
				break;
			}
			ptrace(PTRACE_CONT, child, 0, WSTOPSIG(status));
		}
	}

	if (!seccomp_event)
		goto out;

	/* At SECCOMP stop: modify x0 (fd) from 2 to 1 */
	{
		unsigned long long syscall_nr, x0;
		struct user_pt_regs regs;
		struct iovec iov = { .iov_base = &regs, .iov_len = sizeof(regs) };

		if (ptrace(PTRACE_GETREGSET, child, NT_PRSTATUS, &iov))
			ksft_exit_fail_perror("GETREGSET");

		syscall_nr = regs.regs[8];
		x0 = regs.regs[0];
		if (syscall_nr != __NR_write || x0 != 2) {
			ksft_test_result_fail("bad regs\n");
			goto out;
		}

		regs.regs[0] = 1;
		if (ptrace(PTRACE_SETREGSET, child, NT_PRSTATUS, &iov)) {
			ksft_test_result_fail("SETREGSET\n");
			goto out;
		}
	}

	/* Enable tracepoint and clear buffer */
	clear_trace();
	enable_trace(true);

	/* Continue child; it will execute write(1, ...) and hit tracepoint */
	if (ptrace(PTRACE_CONT, child, 0, 0))
		ksft_exit_fail_perror("PTRACE_CONT after SECCOMP");

	/* Reap the child */
	while (1) {
		if (waitpid(child, &status, 0) != child)
			break;
		if (WIFEXITED(status) || WIFSIGNALED(status))
			break;
		ptrace(PTRACE_CONT, child, 0, WSTOPSIG(status));
	}
	enable_trace(false);

	/* Check trace for the modified fd */
	ok = trace_pipe_expect("fd: 1", 5);
	ksft_test_result(ok, "seccomp_ret_trace_x0_tracepoint\n");

out:
	if (child > 0) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
	ksft_print_cnts();
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
