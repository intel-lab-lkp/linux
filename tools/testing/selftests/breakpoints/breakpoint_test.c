// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 Red Hat, Inc., Frederic Weisbecker <fweisbec@redhat.com>
 *
 * Selftests for breakpoints (and more generally the do_debug() path) in x86.
 */


#include <sys/ptrace.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/user.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

#include "kselftest.h"

#define COUNT_ISN_BPS	4
#define COUNT_WPS	4

/* Force watchpoint access to actually occur */
#define READ_ONCE(x)       (*(volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, val) (*(volatile typeof(x) *)&(x) = (val))

/* Breakpoint access modes */
enum {
	BP_X = 1,
	BP_RW = 2,
	BP_W = 4,
};

static pid_t child_pid;

/*
 * Ensures the child and parent are always "talking" about
 * the same test sequence. (ie: that we haven't forgotten
 * to call check_trapped() somewhere).
 */
static int nr_tests;

static void set_breakpoint_addr(void *addr, int n)
{
	int ret = ptrace(PTRACE_POKEUSER, child_pid,
			 offsetof(struct user, u_debugreg[n]), addr);
	if (ret)
		ksft_exit_fail_msg("Can't set breakpoint addr: %s\n",
			strerror(errno));
}

static void toggle_breakpoint(int n, int type, int len,
			      int local, int global, int set)
{
	int ret;

	int xtype = 0, xlen = 0;
	unsigned long vdr7, dr7;

	switch (type) {
	case BP_X:
		xtype = 0;
		break;
	case BP_W:
		xtype = 1;
		break;
	case BP_RW:
		xtype = 3;
		break;
	}

	switch (len) {
	case 1:
		xlen = 0;
		break;
	case 2:
		xlen = 4;
		break;
	case 4:
		xlen = 0xc;
		break;
	case 8:
		xlen = 8;
		break;
	}

	dr7 = ptrace(PTRACE_PEEKUSER, child_pid,
		     offsetof(struct user, u_debugreg[7]), 0);

	vdr7 = (xlen | xtype) << 16;
	vdr7 <<= 4 * n;

	if (local) {
		vdr7 |= 1 << (2 * n);
		vdr7 |= 1 << 8;
	}
	if (global) {
		vdr7 |= 2 << (2 * n);
		vdr7 |= 1 << 9;
	}

	if (set)
		dr7 |= vdr7;
	else
		dr7 &= ~vdr7;

	ret = ptrace(PTRACE_POKEUSER, child_pid,
		     offsetof(struct user, u_debugreg[7]), dr7);
	if (ret) {
		ksft_print_msg("Can't set dr7: %s\n", strerror(errno));
		exit(-1);
	}
}

/* Dummy variables to test read/write accesses */
static unsigned long long dummy_var[4];

/* Dummy functions to test execution accesses */
static void dummy_func(void) { }
static void dummy_func1(void) { }
static void dummy_func2(void) { }
static void dummy_func3(void) { }

static void (*dummy_funcs[])(void) = {
	dummy_func,
	dummy_func1,
	dummy_func2,
	dummy_func3,
};

static int trapped;

static void check_trapped(void)
{
	/*
	 * If we haven't trapped, wake up the parent
	 * so that it notices the failure.
	 */
	if (!READ_ONCE(trapped))
		kill(getpid(), SIGUSR1);
	WRITE_ONCE(trapped, 0);

	nr_tests++;
}

static void write_var(int len)
{
	int i;

	for (i = 0; i < 4; i++) {
		switch (len) {
		case 1:
			WRITE_ONCE(*(char *)&dummy_var[i], 0xff);
			break;
		case 2:
			WRITE_ONCE(*(short *)&dummy_var[i], 0xffff);
			break;
		case 4:
			WRITE_ONCE(*(int *)&dummy_var[i], 0xffffffff);
			break;
		case 8:
			WRITE_ONCE(*(long long *)&dummy_var[i], 0xffffffffffffffffLL);
			break;
		}
		check_trapped();
	}
}

static void read_var(int len)
{
	int i;

	for (i = 0; i < 4; i++) {
		switch (len) {
		case 1:
			(void)READ_ONCE(*(char *)&dummy_var[i]);
			break;
		case 2:
			(void)READ_ONCE(*(short *)&dummy_var[i]);
			break;
		case 4:
			(void)READ_ONCE(*(int *)&dummy_var[i]);
			break;
		case 8:
			(void)READ_ONCE(*(long long *)&dummy_var[i]);
			break;
		}
		check_trapped();
	}
}

/*
 * Do the r/w/x accesses to trigger the breakpoints. And run
 * the usual traps.
 */
static void trigger_tests(void)
{
	size_t len, local, global, i;
	int ret = ptrace(PTRACE_TRACEME, 0, NULL, 0);
	if (ret) {
		ksft_print_msg("Can't be traced? %s\n", strerror(errno));
		return;
	}

	/* Wake up father so that it sets up the first test */
	kill(getpid(), SIGUSR1);

	/* Test instruction breakpoints */
	for (local = 0; local < 2; local++) {
		for (global = 0; global < 2; global++) {
			if (!local && !global)
				continue;

			for (i = 0; i < COUNT_ISN_BPS; i++) {
				dummy_funcs[i]();
				check_trapped();
			}
		}
	}

	/* Test write watchpoints */
	for (len = 1; len <= sizeof(long); len <<= 1) {
		for (local = 0; local < 2; local++) {
			for (global = 0; global < 2; global++) {
				if (!local && !global)
					continue;
				write_var(len);
			}
		}
	}

	/* Test read/write watchpoints (on read accesses) */
	for (len = 1; len <= sizeof(long); len <<= 1) {
		for (local = 0; local < 2; local++) {
			for (global = 0; global < 2; global++) {
				if (!local && !global)
					continue;
				read_var(len);
			}
		}
	}

	/* Icebp trap */
	__asm__ __volatile__ (".byte 0xf1\n");
	check_trapped();

	/* Int 3 trap */
	__asm__ __volatile__ ("int $3\n");
	check_trapped();

	kill(getpid(), SIGUSR1);
}

static void check_success(const char *msg)
{
	int status;
	int ret = 0;

	/* Wait for the child to SIGTRAP */
	wait(&status);

	if (WSTOPSIG(status) == SIGTRAP) {
		int child_nr_tests = ptrace(PTRACE_PEEKDATA, child_pid,
					    &nr_tests, 0);
		if (child_nr_tests == nr_tests)
			ret = 1;
		if (ptrace(PTRACE_POKEDATA, child_pid, &trapped, 1))
			ksft_exit_fail_msg("Can't poke: %s\n", strerror(errno));
	}

	nr_tests++;

	if (ret)
		ksft_test_result_pass("%s", msg);
	else
		ksft_test_result_fail("%s", msg);
}

static void launch_instruction_breakpoints(char *buf, int local, int global)
{
	int i;

	for (i = 0; i < COUNT_ISN_BPS; i++) {
		set_breakpoint_addr(dummy_funcs[i], i);
		toggle_breakpoint(i, BP_X, 1, local, global, 1);
		ptrace(PTRACE_CONT, child_pid, NULL, 0);
		sprintf(buf, "Test breakpoint %d with local: %d global: %d\n",
			i, local, global);
		check_success(buf);
		toggle_breakpoint(i, BP_X, 1, local, global, 0);
	}
}

static void launch_watchpoints(char *buf, int mode, int len,
			       int local, int global)
{
	const char *mode_str;
	int i;

	if (mode == BP_W)
		mode_str = "write";
	else
		mode_str = "read";

	for (i = 0; i < COUNT_WPS; i++) {
		set_breakpoint_addr(&dummy_var[i], i);
		toggle_breakpoint(i, mode, len, local, global, 1);
		ptrace(PTRACE_CONT, child_pid, NULL, 0);
		sprintf(buf,
			"Test %s watchpoint %d with len: %d local: %d global: %d\n",
			mode_str, i, len, local, global);
		check_success(buf);
		toggle_breakpoint(i, mode, len, local, global, 0);
	}
}

/* Set the breakpoints and check the child successfully trigger them */
static void launch_tests(void)
{
	char buf[1024];
	unsigned int tests = 0;
	size_t len, local, global;

	tests += 3 * COUNT_ISN_BPS;
	tests += sizeof(long) / 2 * 3 * COUNT_WPS;
	tests += sizeof(long) / 2 * 3 * COUNT_WPS;
	tests += 2;
	ksft_set_plan(tests);

	/* Instruction breakpoints */
	for (local = 0; local < 2; local++) {
		for (global = 0; global < 2; global++) {
			if (!local && !global)
				continue;
			launch_instruction_breakpoints(buf, local, global);
		}
	}

	/* Write watchpoint */
	for (len = 1; len <= sizeof(long); len <<= 1) {
		for (local = 0; local < 2; local++) {
			for (global = 0; global < 2; global++) {
				if (!local && !global)
					continue;
				launch_watchpoints(buf, BP_W, len,
						   local, global);
			}
		}
	}

	/* Read-Write watchpoint */
	for (len = 1; len <= sizeof(long); len <<= 1) {
		for (local = 0; local < 2; local++) {
			for (global = 0; global < 2; global++) {
				if (!local && !global)
					continue;
				launch_watchpoints(buf, BP_RW, len,
						   local, global);
			}
		}
	}

	/* Icebp traps */
	ptrace(PTRACE_CONT, child_pid, NULL, 0);
	check_success("Test icebp\n");

	/* Int 3 traps */
	ptrace(PTRACE_CONT, child_pid, NULL, 0);
	check_success("Test int 3 trap\n");

	ptrace(PTRACE_CONT, child_pid, NULL, 0);
}

int main(void)
{
	pid_t pid;

	ksft_print_header();

	pid = fork();
	if (!pid) {
		trigger_tests();
		exit(0);
	}

	child_pid = pid;

	wait(NULL);

	launch_tests();

	wait(NULL);

	ksft_exit_pass();
}
