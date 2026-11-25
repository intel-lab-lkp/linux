// SPDX-License-Identifier: GPL-2.0
/*
 * ia32_emulation_param.c - Test the ia32_emulation= boot parameter
 *
 * Probes INT $0x80 and execve() of a 32-bit executable. They must
 * either both work or both be refused, and this must match
 * ia32_emulation= on the kernel command line, if it is there.
 *
 * Usage: ia32_emulation_param [32-bit helper that exits with 0]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "kselftest.h"

#define PARAM "ia32_emulation="
#define IA32_NR_GETPID 20
#define CHILD32_EXIT 0
/* If execve() fails, the child exits with EXEC_FAILED + errno. */
#define EXEC_FAILED 100

static sigjmp_buf jmpbuf;

static void catch_sig(int sig)
{
	siglongjmp(jmpbuf, sig);
}

static void set_handler(int sig, void (*handler)(int))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, NULL))
		ksft_exit_fail_msg("sigaction: %s\n", strerror(errno));
}

/* Value of the last ia32_emulation= on the command line, or -1. */
static int param_value(void)
{
	char buf[4096], *p, *last = NULL;
	FILE *f = fopen("/proc/cmdline", "r");

	if (!f)
		ksft_exit_fail_msg("open /proc/cmdline: %s\n", strerror(errno));
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);

	for (p = strstr(buf, PARAM); p; p = strstr(p + 1, PARAM))
		if (p == buf || p[-1] == ' ')
			last = p + strlen(PARAM);
	if (!last)
		return -1;

	switch (last[0]) {
	case 'y':
	case 'Y':
	case 't':
	case 'T':
	case '1':
		return 1;
	case 'n':
	case 'N':
	case 'f':
	case 'F':
	case '0':
		return 0;
	case 'o':
	case 'O':
		return last[1] == 'n' || last[1] == 'N';
	}
	return -1;
}

/* Result of getpid() via INT $0x80, or -1 with *sig set if a signal came. */
static long try_int80(int *sig)
{
	long ret = -1;

	*sig = 0;
	set_handler(SIGSEGV, catch_sig);
	set_handler(SIGBUS, catch_sig);
	switch (sigsetjmp(jmpbuf, 1)) {
	case 0:
		asm volatile("int $0x80"
			: "=a" (ret)
			: "a" (IA32_NR_GETPID)
			: "memory", "r8", "r9", "r10", "r11");
		break;
	case SIGSEGV:
		*sig = SIGSEGV;
		ret = -1;
		break;
	default:
		*sig = SIGBUS;
		ret = -1;
		break;
	}
	set_handler(SIGSEGV, SIG_DFL);
	set_handler(SIGBUS, SIG_DFL);
	return ret;
}

/* Exit status of the child, or -1 if it did not exit normally. */
static int try_exec32(const char *path)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (pid == 0) {
		execl(path, path, (char *)NULL);
		_exit(EXEC_FAILED + errno);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

static void check(int expect, bool works, const char *name)
{
	if (expect < 0)
		ksft_test_result_skip("%s: no %s on the command line\n", name, PARAM);
	else if (works == !!expect)
		ksft_test_result_pass("%s\n", name);
	else
		ksft_test_result_fail("%s\n", name);
}

int main(int argc, char **argv)
{
	const char *child32 = argc > 1 ? argv[1] : "./ia32_emulation_child_32";
	bool int80_ok, exec_ok = false, have_child;
	int expect, rex, sig;
	long r80;

	ksft_print_header();
	ksft_set_plan(3);

	expect = param_value();
	if (expect < 0)
		ksft_print_msg("no %s on the command line\n", PARAM);
	else
		ksft_print_msg("%s%d\n", PARAM, expect);

	r80 = try_int80(&sig);
	int80_ok = !sig && (int)r80 == getpid();
	if (sig)
		ksft_print_msg("INT $0x80 killed by signal %d\n", sig);
	else
		ksft_print_msg("INT $0x80 returned %ld, pid is %d\n", r80, getpid());

	have_child = access(child32, X_OK) == 0;
	if (!have_child) {
		ksft_print_msg("%s not found, skipping the execve() checks\n", child32);
	} else {
		rex = try_exec32(child32);
		exec_ok = rex == CHILD32_EXIT;
		if (rex >= EXEC_FAILED)
			ksft_print_msg("execve(%s) failed: %s\n", child32,
				       strerror(rex - EXEC_FAILED));
		else if (rex < 0)
			ksft_print_msg("child did not exit normally\n");
		else
			ksft_print_msg("%s exited with %d\n", child32, rex);
	}

	check(expect, int80_ok, "INT $0x80 works");
	if (!have_child) {
		ksft_test_result_skip("execve() of a 32-bit binary works: no 32-bit helper\n");
		ksft_test_result_skip("INT $0x80 and execve() agree: no 32-bit helper\n");
		ksft_finished();
	}
	check(expect, exec_ok, "execve() of a 32-bit binary works");
	if (int80_ok == exec_ok)
		ksft_test_result_pass("INT $0x80 and execve() agree\n");
	else
		ksft_test_result_fail("INT $0x80 and execve() agree\n");

	ksft_finished();
}
