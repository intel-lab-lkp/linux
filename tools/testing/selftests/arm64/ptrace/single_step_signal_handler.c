// SPDX-License-Identifier: GPL-2.0
/*
 * Single-stepping into a signal handler must stop at the handler entry.
 *
 * When TIF_SINGLESTEP is set the kernel must arrange for the tracee to be
 * stopped as it steps into a signal handler; leaving it to the hardware
 * fast-forward would execute the first instruction of the handler before
 * reporting it.
 *
 * (ac2081cdc4d9 "arm64: ptrace: Consistently use pseudo-singlestep
 * exceptions")
 */
#include <sys/mman.h>

#include "kselftest.h"
#include "ptrace_common.h"

static volatile unsigned long *sh;

static void sigusr1_handler(int sig)
{
	(void)sig;
	sh[1] = 1;
}

static bool steps_into_signal_handler(void)
{
	int status, deliver = 0, delivering = 0;
	bool stepped_in = false, handler_ran = false;
	bool ok = false;
	pid_t child;

	sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		return false;
	sh[0] = 0;
	sh[1] = 0;

	child = fork();
	if (child == 0) {
		struct sigaction sa;
		sigset_t blk;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sigusr1_handler;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR1, &sa, NULL);

		sigemptyset(&blk);
		sigaddset(&blk, SIGTRAP);
		sigprocmask(SIG_BLOCK, &blk, NULL);

		if (traceme() < 0)
			_exit(3);
		sh[0] = (unsigned long)sigusr1_handler;
		raise(SIGSTOP);
		while (!sh[1])
			asm volatile("nop");
		_exit(0);
	}

	if (waitpid(child, &status, 0) < 0)
		return false;
	ptrace(PTRACE_SETOPTIONS, child, 0, 0);

	/* Queue the signal; it is delivered on the next resume. */
	kill(child, SIGUSR1);
	ptrace_timeout_start(60);

	for (;;) {
		int sig;
		unsigned long pc;

		if (ptrace_timed_out)
			break;
		if (ptrace(PTRACE_SINGLESTEP, child, 0,
			   (void *)(long)deliver) < 0)
			break;
		deliver = 0;
		if (waitpid(child, &status, 0) < 0)
			break;
		if (WIFEXITED(status) || WIFSIGNALED(status))
			break;
		if (!WIFSTOPPED(status))
			continue;

		sig = WSTOPSIG(status);
		pc = tracee_pc(child);

		if (sig == SIGUSR1) {
			/* deliver it on the next resume */
			deliver = SIGUSR1;
			delivering = 1;
			continue;
		}
		if (delivering) {
			/* the kernel must stop at the handler entry */
			if (sig != SIGTRAP || pc != sh[0])
				break;
			stepped_in = true;
			delivering = 0;
			continue;
		}
		if (stepped_in && sh[1]) {
			/* the handler ran, so it was not reset to SIG_DFL */
			handler_ran = true;
			break;
		}
	}
	ptrace_timeout_stop();
	tracee_kill(child);

	ok = stepped_in && handler_ran;
	return ok;
}

int main(void)
{
	bool ok;

	ksft_print_header();
	ksft_set_plan(1);

	ok = steps_into_signal_handler();
	ksft_test_result(ok, "stopped at the signal handler entry\n");

	if (ok)
		ksft_exit_pass();
	ksft_exit_fail();
}
