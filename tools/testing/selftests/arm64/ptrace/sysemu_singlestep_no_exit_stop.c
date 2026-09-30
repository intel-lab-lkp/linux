// SPDX-License-Identifier: GPL-2.0
/*
 * An emulated system call (PTRACE_SYSEMU_SINGLESTEP) must report a
 * syscall-enter-stop but no syscall-exit-stop, as documented in ptrace(2):
 *
 *	"If the tracee was restarted by PTRACE_SYSCALL or PTRACE_SYSEMU,
 *	 the tracee enters syscall-enter-stop just prior to entering any
 *	 system call ... If the tracee is continued using any other method
 *	 (including PTRACE_SYSEMU), no syscall-exit-stop occurs."
 */
#include <sys/syscall.h>

#include "kselftest.h"
#include "ptrace_common.h"

static bool emulated_syscall_has_no_exit_stop(void)
{
	pid_t child;
	int status, after = -1;
	bool saw_entry = false, saw_exit = false;

	child = fork();
	if (child == 0) {
		if (traceme() < 0)
			_exit(3);
		raise(SIGSTOP);
		(void)syscall(SYS_getpid);
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
		if (op == PTRACE_SYSCALL_INFO_ENTRY) {
			saw_entry = true;
			if (nr == SYS_getpid && after < 0)
				after = 4;
		}
		if (op == PTRACE_SYSCALL_INFO_EXIT)
			saw_exit = true;
		if (after > 0 && --after == 0)
			break;
	}
	ptrace_timeout_stop();
	tracee_kill(child);

	return saw_entry && !saw_exit;
}

int main(void)
{
	bool ok;

	ksft_print_header();
	ksft_set_plan(1);

	ok = emulated_syscall_has_no_exit_stop();
	ksft_test_result(ok, "emulated syscall has no syscall-exit-stop\n");

	if (ok)
		ksft_exit_pass();
	ksft_exit_fail();
}
