// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2020 Bernd Edlinger <bernd.edlinger@hotmail.de>
 * All rights reserved.
 *
 * Check whether /proc/$pid/mem can be accessed without causing deadlocks
 * when de_thread is blocked with ->cred_guard_mutex held.
 */

#include "../kselftest_harness.h"
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>

static void *thread(void *arg)
{
	ptrace(PTRACE_TRACEME, 0, 0L, 0L);
	return NULL;
}

TEST(vmaccess)
{
	int s, f, pid = fork();
	char mm[64];

	if (!pid) {
		pthread_t pt;

		pthread_create(&pt, NULL, thread, NULL);
		pthread_join(pt, NULL);
		execlp("false", "false", NULL);
		return;
	}

	sleep(1);
	sprintf(mm, "/proc/%d/mem", pid);
	/* deadlock did happen here */
	f = open(mm, O_RDONLY);
	ASSERT_GE(f, 0);
	close(f);
	f = waitpid(-1, &s, WNOHANG);
	ASSERT_NE(f, -1);
	ASSERT_NE(f, 0);
	ASSERT_NE(f, pid);
	ASSERT_EQ(WIFEXITED(s), 1);
	ASSERT_EQ(WEXITSTATUS(s), 0);
	f = waitpid(-1, &s, 0);
	ASSERT_EQ(f, pid);
	ASSERT_EQ(WIFEXITED(s), 1);
	ASSERT_EQ(WEXITSTATUS(s), 1);
	f = waitpid(-1, NULL, 0);
	ASSERT_EQ(f, -1);
	ASSERT_EQ(errno, ECHILD);
}

/*
 * Same test as previous, except that
 * we try to ptrace the group leader,
 * which is about to call execve,
 * when the other thread is already ptraced.
 * This exercises the code in de_thread
 * where it is waiting inside the
 * while (sig->notify_count) {
 * loop.
 */
TEST(attach1)
{
	int s, k, pid = fork();

	if (!pid) {
		pthread_t pt;

		pthread_create(&pt, NULL, thread, NULL);
		pthread_join(pt, NULL);
		execlp("false", "false", NULL);
		return;
	}

	sleep(1);
	/* deadlock may happen here */
	k = ptrace(PTRACE_ATTACH, pid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_NE(k, -1);
	ASSERT_NE(k, 0);
	ASSERT_NE(k, pid);
	ASSERT_EQ(WIFEXITED(s), 1);
	ASSERT_EQ(WEXITSTATUS(s), 0);
	k = waitpid(-1, &s, 0);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFSTOPPED(s), 1);
	ASSERT_EQ(WSTOPSIG(s), SIGTRAP);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_EQ(k, 0);
	k = ptrace(PTRACE_CONT, pid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, 0);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFSTOPPED(s), 1);
	ASSERT_EQ(WSTOPSIG(s), SIGSTOP);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_EQ(k, 0);
	k = ptrace(PTRACE_CONT, pid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, 0);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFEXITED(s), 1);
	ASSERT_EQ(WEXITSTATUS(s), 1);
	k = waitpid(-1, NULL, 0);
	ASSERT_EQ(k, -1);
	ASSERT_EQ(errno, ECHILD);
}

/*
 * Same test as previous, except that
 * the group leader is ptraced first,
 * but this time with PTRACE_O_TRACEEXIT,
 * and the thread that does execve is
 * not yet ptraced.  This exercises the
 * code block in de_thread where the
 * if (!thread_group_leader(tsk)) {
 * is executed and enters a wait state.
 */
static long thread2_tid;
static void *thread2(void *arg)
{
	thread2_tid = syscall(__NR_gettid);
	sleep(2);
	execlp("false", "false", NULL);
	return NULL;
}

TEST(attach2)
{
	int s, k, pid = fork();

	if (!pid) {
		pthread_t pt;

		pthread_create(&pt, NULL, thread2, NULL);
		pthread_join(pt, NULL);
		return;
	}

	sleep(1);
	k = ptrace(PTRACE_ATTACH, pid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, 0);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFSTOPPED(s), 1);
	ASSERT_EQ(WSTOPSIG(s), SIGSTOP);
	k = ptrace(PTRACE_SETOPTIONS, pid, 0L, PTRACE_O_TRACEEXIT);
	ASSERT_EQ(k, 0);
	thread2_tid = ptrace(PTRACE_PEEKDATA, pid, &thread2_tid, 0L);
	ASSERT_NE(thread2_tid, -1);
	ASSERT_NE(thread2_tid, 0);
	ASSERT_NE(thread2_tid, pid);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_EQ(k, 0);
	sleep(2);
	/* deadlock may happen here */
	k = ptrace(PTRACE_ATTACH, thread2_tid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFSTOPPED(s), 1);
	ASSERT_EQ(WSTOPSIG(s), SIGTRAP);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_EQ(k, 0);
	k = ptrace(PTRACE_CONT, pid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, 0);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFSTOPPED(s), 1);
	ASSERT_EQ(WSTOPSIG(s), SIGTRAP);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_EQ(k, 0);
	k = ptrace(PTRACE_CONT, pid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, 0);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFSTOPPED(s), 1);
	ASSERT_EQ(WSTOPSIG(s), SIGSTOP);
	k = waitpid(-1, &s, WNOHANG);
	ASSERT_EQ(k, 0);
	k = ptrace(PTRACE_CONT, pid, 0L, 0L);
	ASSERT_EQ(k, 0);
	k = waitpid(-1, &s, 0);
	ASSERT_EQ(k, pid);
	ASSERT_EQ(WIFEXITED(s), 1);
	ASSERT_EQ(WEXITSTATUS(s), 1);
	k = waitpid(-1, NULL, 0);
	ASSERT_EQ(k, -1);
	ASSERT_EQ(errno, ECHILD);
}

TEST_HARNESS_MAIN
