// SPDX-License-Identifier: GPL-2.0
/*
 * A ptrace stop taken during a system call (here a seccomp RET_TRACE trap)
 * must not lose the single-step stop at syscall exit, even if the tracer
 * modifies the tracee's registers at that stop.
 *
 * This covers the second issue fixed by ac2081cdc4d9 ("arm64: ptrace:
 * Consistently use pseudo-singlestep exceptions"): the hardware
 * fast-forwards the software-step state machine across the SVC, and the
 * ptrace stop during the syscall may rewind it.
 */
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <linux/filter.h>
#include <linux/seccomp.h>

#include "kselftest.h"
#include "ptrace_common.h"

static int install_seccomp_getpid_trace(void)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getpid, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = (unsigned short)ARRAY_SIZE(filter),
		.filter = filter,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		return -1;
	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

static void poke_a_register(pid_t pid)
{
	struct user_pt_regs regs;
	struct iovec iov;

	iov.iov_base = &regs;
	iov.iov_len = sizeof(regs);
	if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) == 0) {
		regs.regs[0] ^= 1;
		ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
	}
}

static bool step_survives_stop_during_syscall(void)
{
	volatile int *marker;
	pid_t child;
	int status, seen_seccomp = 0;
	bool ok = false;

	marker = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (marker == MAP_FAILED)
		return false;
	*marker = 0;

	child = fork();
	if (child == 0) {
		if (traceme() < 0)
			_exit(3);
		raise(SIGSTOP);
		if (install_seccomp_getpid_trace() < 0)
			_exit(2);
		(void)syscall(SYS_getpid);	/* seccomp RET_TRACE trap */
		*marker = 1;			/* next instruction */
		_exit(0);
	}

	if (waitpid(child, &status, 0) < 0)
		return false;
	if (WIFEXITED(status))
		return false;
	ptrace(PTRACE_SETOPTIONS, child, 0, (void *)PTRACE_O_TRACESECCOMP);
	ptrace_timeout_start(60);

	for (;;) {
		if (ptrace_timed_out)
			break;
		if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) < 0)
			break;
		if (waitpid(child, &status, 0) < 0)
			break;
		if (WIFEXITED(status) || WIFSIGNALED(status))
			break;
		if (!WIFSTOPPED(status))
			continue;

		if ((status >> 8) ==
		    (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))) {
			seen_seccomp = 1;
			poke_a_register(child);
			continue;
		}
		if (seen_seccomp) {
			/* first stop after the syscall, before the marker */
			ok = (*marker == 0);
			break;
		}
	}
	ptrace_timeout_stop();
	tracee_kill(child);

	return ok;
}

int main(void)
{
	bool ok;

	ksft_print_header();
	ksft_set_plan(1);

	ok = step_survives_stop_during_syscall();
	ksft_test_result(ok, "single-step survives a ptrace stop during syscall\n");

	if (ok)
		ksft_exit_pass();
	ksft_exit_fail();
}
