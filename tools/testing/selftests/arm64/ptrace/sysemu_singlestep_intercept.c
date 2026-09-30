// SPDX-License-Identifier: GPL-2.0
/*
 * After an emulated system call, the tracee must be intercepted by a
 * single-step stop before it executes the next instruction.  This guards
 * against the software-step state machine being lost around the SVC.
 */
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest.h"
#include "ptrace_common.h"

static bool intercepted_after_emulated_syscall(void)
{
	volatile int *marker;
	pid_t child;
	int status, phase = 0;
	bool intercepted = false;

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
		(void)syscall(SYS_getpid);	/* emulated syscall */
		*marker = 1;			/* next instruction */
		(void)syscall(SYS_getpid);	/* endpoint */
		_exit(0);
	}

	if (waitpid(child, &status, 0) < 0)
		return false;
	ptrace(PTRACE_SETOPTIONS, child, 0, (void *)PTRACE_O_TRACESYSGOOD);
	ptrace_timeout_start(60);

	for (;;) {
		long nr = -1;
		int op;

		if (ptrace_timed_out)
			break;
		if (ptrace(PTRACE_SYSEMU_SINGLESTEP, child, 0, 0) < 0)
			break;
		if (waitpid(child, &status, 0) < 0)
			break;
		if (WIFEXITED(status))
			break;
		if (!WIFSTOPPED(status))
			continue;

		op = syscall_info(child, &nr);

		if (op == PTRACE_SYSCALL_INFO_ENTRY && nr == SYS_getpid &&
		    phase == 0) {
			/* reached the emulated syscall, resume once */
			phase = 1;
			continue;
		}

		if (phase == 1) {
			/* First stop after the emulated syscall: the marker
			 * must not have run yet, i.e. the step was delivered.
			 */
			intercepted = (*marker == 0);
			break;
		}
	}
	ptrace_timeout_stop();
	tracee_kill(child);

	return intercepted;
}

int main(void)
{
	bool ok;

	ksft_print_header();
	ksft_set_plan(1);

	ok = intercepted_after_emulated_syscall();
	ksft_test_result(ok, "tracee intercepted after the emulated syscall\n");

	if (ok)
		ksft_exit_pass();
	ksft_exit_fail();
}
