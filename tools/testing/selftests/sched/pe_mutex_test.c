// SPDX-License-Identifier: GPL-2.0-only
/*
 * Proxy Execution (PE) mutex selftest - TC-1 through TC-3
 *
 * Verifies basic PE behavior for mutex blocking:
 *   TC-1: High-priority blocked task's CPU time increases via PE
 *   TC-2: blocked_on lifetime - voluntary ctxt switches don't increase
 *   TC-3: Two-level mutex chain traversal
 *
 * Requires CONFIG_SCHED_PROXY_EXEC=y and root privileges.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "../kselftest.h"

/* ---------- helpers ---------- */

static pid_t gettid_compat(void)
{
	return (pid_t)syscall(SYS_gettid);
}

/*
 * is_proxy_exec_enabled - check whether PE is active at runtime.
 *
 * PE has no sysctl; it is controlled by the "sched_proxy_exec" boot
 * parameter.  DEFINE_STATIC_KEY_TRUE means it defaults ON unless
 * "sched_proxy_exec=0" appears on the kernel command line.
 */
static bool is_proxy_exec_enabled(void)
{
	char line[4096];
	FILE *f;

	f = fopen("/proc/cmdline", "r");
	if (!f)
		return true; /* assume enabled if we cannot read cmdline */

	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return true;
	}
	fclose(f);

	return !strstr(line, "sched_proxy_exec=0");
}

/* Return monotonic time in nanoseconds. */
static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Return CPU time consumed by the calling thread in nanoseconds. */
static long long cputime_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * get_voluntary_ctxt_switches - read voluntary_ctxt_switches for @tid.
 *
 * Threads (tid != tgid) are only visible under
 * /proc/<tgid>/task/<tid>/status, not /proc/<tid>/status directly.
 * Try the task path first, fall back to the top-level pid path.
 */
static long get_voluntary_ctxt_switches(pid_t tid)
{
	char path[128];
	char line[256];
	FILE *f;
	long val = -1;

	/* Try /proc/<tgid>/task/<tid>/status (works for all threads) */
	snprintf(path, sizeof(path), "/proc/%d/task/%d/status",
		 (int)getpid(), (int)tid);
	f = fopen(path, "r");
	if (!f) {
		/* Fallback: /proc/<tid>/status (works only for tgid == tid) */
		snprintf(path, sizeof(path), "/proc/%d/status", (int)tid);
		f = fopen(path, "r");
	}
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
			val = strtol(line + 24, NULL, 10);
			break;
		}
	}
	fclose(f);
	return val;
}

/* Set SCHED_FIFO priority for the calling thread. */
static int set_fifo(int prio)
{
	struct sched_param sp = { .sched_priority = prio };

	return sched_setscheduler(0, SCHED_FIFO, &sp);
}

/* Set SCHED_OTHER (normal) for the calling thread. */
static int set_normal(void)
{
	struct sched_param sp = { .sched_priority = 0 };

	return sched_setscheduler(0, SCHED_OTHER, &sp);
}

/* ---------- TC-1 ----------------------------------------------------------
 *
 * Single-level PE: high-priority waiter gets CPU via PE.
 *
 * Setup:
 *   - LOW thread (SCHED_OTHER): holds mutex, burns CPU for ~200 ms,
 *     then releases.
 *   - HIGH thread (SCHED_FIFO prio=80): waits for mutex immediately.
 *
 * On a PE kernel the scheduler runs LOW as proxy for HIGH, so LOW
 * should accumulate significant CPU time (measured via
 * CLOCK_PROCESS_CPUTIME_ID inside the holder thread itself).
 *
 * Verification: CPU time consumed by the LOW thread during the hold
 * period is >= 50 ms.  CLOCK_THREAD_CPUTIME_ID is used so that only
 * LOW's own CPU consumption is measured, not that of other threads.
 */

#define TC1_HOLD_MS 200 /* ms LOW holds the mutex */
#define TC1_CPU_THRESHOLD_MS 50 /* minimum CPU ms we expect */

struct tc1_args {
	pthread_mutex_t *mtx;
	long long cpu_during_hold_ns; /* output: CPU ns consumed by LOW */
	atomic_int ready;
	atomic_int done;
};

static void *tc1_low_thread(void *arg)
{
	struct tc1_args *a = arg;
	long long t0, t1, deadline;

	/* Become the LOW thread */
	set_normal();

	pthread_mutex_lock(a->mtx);
	a->ready = 1;

	/* Spin for TC1_HOLD_MS real-time milliseconds while holding lock */
	deadline = now_ns() + (long long)TC1_HOLD_MS * 1000000LL;
	t0 = cputime_ns();
	while (now_ns() < deadline)
		; /* busy wait */
	t1 = cputime_ns();

	a->cpu_during_hold_ns = t1 - t0;
	pthread_mutex_unlock(a->mtx);
	a->done = 1;
	return NULL;
}

static void *tc1_high_thread(void *arg)
{
	struct tc1_args *a = arg;

	/* Become HIGH priority */
	set_fifo(80);

	/* Wait until LOW has the lock */
	while (!a->ready)
		sched_yield();

	/* Block on mutex - PE should now proxy-run LOW */
	pthread_mutex_lock(a->mtx);
	pthread_mutex_unlock(a->mtx);
	return NULL;
}

static void run_tc1(void)
{
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	struct tc1_args args = { .mtx = &mtx, .ready = 0, .done = 0 };
	pthread_t low, high;
	long long threshold_ns = (long long)TC1_CPU_THRESHOLD_MS * 1000000LL;

	pthread_create(&low, NULL, tc1_low_thread, &args);

	/* Wait for LOW to acquire the lock before creating HIGH */
	while (!args.ready)
		sched_yield();

	pthread_create(&high, NULL, tc1_high_thread, &args);

	pthread_join(high, NULL);
	pthread_join(low, NULL);

	pthread_mutex_destroy(&mtx);

	if (args.cpu_during_hold_ns >= threshold_ns) {
		ksft_test_result_pass(
			"TC-1: PE ran LOW as proxy (cpu_hold=%lld ms >= %d ms)\n",
			args.cpu_during_hold_ns / 1000000,
			TC1_CPU_THRESHOLD_MS);
	} else {
		ksft_test_result_fail(
			"TC-1: LOW did not get enough CPU time (cpu_hold=%lld ms < %d ms)\n",
			args.cpu_during_hold_ns / 1000000,
			TC1_CPU_THRESHOLD_MS);
	}
}

/* ---------- TC-2 ----------------------------------------------------------
 *
 * blocked_on lifetime: voluntary context switches must NOT increase
 * for the high-priority waiter while it is proxy-blocked.
 *
 * When PE is active the high-priority task stays on the runqueue
 * (as donor) and is never voluntarily context-switched out.
 *
 * Verification:
 *   Record voluntary_ctxt_switches for HIGH before and after the
 *   blocking period; they should be equal.
 */

#define TC2_HOLD_MS 150

struct tc2_args {
	pthread_mutex_t *mtx;
	pid_t high_tid;
	atomic_int low_has_lock;  /* LOW signals it holds the mutex */
	atomic_int high_blocking; /* HIGH signals it is about to block */
	long ctxt_after;          /* HIGH records its own switches after unblock */
};

static void *tc2_low_thread(void *arg)
{
	struct tc2_args *a = arg;
	long long deadline;

	set_normal();
	pthread_mutex_lock(a->mtx);
	a->low_has_lock = 1;

	deadline = now_ns() + (long long)TC2_HOLD_MS * 1000000LL;
	while (now_ns() < deadline)
		; /* busy spin holding the lock */

	pthread_mutex_unlock(a->mtx);
	return NULL;
}

static void *tc2_high_thread(void *arg)
{
	struct tc2_args *a = arg;

	set_fifo(80);
	a->high_tid = gettid_compat();

	/* Wait until LOW holds the lock */
	while (!a->low_has_lock)
		sched_yield();

	/* Signal main that we are about to block, then immediately block */
	a->high_blocking = 1;
	pthread_mutex_lock(a->mtx);
	pthread_mutex_unlock(a->mtx);
	/* Record our own ctxt switches before exiting (proc entry still live) */
	a->ctxt_after = get_voluntary_ctxt_switches(gettid_compat());
	return NULL;
}

static void run_tc2(void)
{
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	struct tc2_args args = {
		.mtx = &mtx,
		.high_tid = 0,
		.low_has_lock = 0,
		.high_blocking = 0,
		.ctxt_after = -1,
	};
	pthread_t low, high;
	long before, after = -1;

	/* Start LOW first so it grabs the lock */
	pthread_create(&low, NULL, tc2_low_thread, &args);

	while (!args.low_has_lock)
		sched_yield();

	pthread_create(&high, NULL, tc2_high_thread, &args);

	/*
	 * Wait until HIGH has set high_tid AND signaled it is about to block.
	 * There is a tiny window between high_blocking=1 and the actual
	 * pthread_mutex_lock() call, but that is unavoidable in userspace.
	 * Sample "before" here; HIGH cannot have voluntarily yielded yet
	 * because it has not blocked yet.
	 */
	while (!args.high_tid || !args.high_blocking)
		sched_yield();

	/* Sample voluntary switches while HIGH is (about to be) blocked */
	before = get_voluntary_ctxt_switches(args.high_tid);

	pthread_join(high, NULL);
	pthread_join(low, NULL);

	after = args.ctxt_after;

	pthread_mutex_destroy(&mtx);

	if (before < 0 || after < 0) {
		ksft_test_result_skip(
			"TC-2: Could not read /proc task status\n");
		return;
	}

	if (after == before) {
		ksft_test_result_pass(
			"TC-2: HIGH voluntary_ctxt_switches unchanged (%ld) during PE block\n",
			before);
	} else {
		ksft_test_result_fail(
			"TC-2: HIGH voluntary_ctxt_switches changed: before=%ld after=%ld\n",
			before, after);
	}
}

/* ---------- TC-3 ----------------------------------------------------------
 *
 * Two-level mutex chain:
 *   A (SCHED_FIFO prio=80) -> blocked on mutex1 -> held by
 *   B (SCHED_FIFO prio=50) -> blocked on mutex2 -> held by
 *   C (SCHED_OTHER)                                  ^^ PE must traverse
 *                                                      the chain and run C
 *
 * Verification: C's CPU time during the hold period is >= 50 ms,
 * meaning PE reached the end of the chain and ran C as proxy.
 */

#define TC3_HOLD_MS 200
#define TC3_CPU_THRESHOLD_MS 50

struct tc3_args {
	pthread_mutex_t *mtx1; /* A waits on this; B holds */
	pthread_mutex_t *mtx2; /* B waits on this; C holds */

	atomic_int b_has_mtx1; /* B has acquired mtx1 */
	atomic_int c_has_mtx2; /* C has acquired mtx2 */

	long long c_cpu_during_hold_ns;
};

static void *tc3_c_thread(void *arg)
{
	struct tc3_args *a = arg;
	long long t0, t1, deadline;

	set_normal();
	pthread_mutex_lock(a->mtx2);
	a->c_has_mtx2 = 1;

	/* Spin holding mtx2 */
	deadline = now_ns() + (long long)TC3_HOLD_MS * 1000000LL;
	t0 = cputime_ns();
	while (now_ns() < deadline)
		;
	t1 = cputime_ns();

	a->c_cpu_during_hold_ns = t1 - t0;
	pthread_mutex_unlock(a->mtx2);
	return NULL;
}

static void *tc3_b_thread(void *arg)
{
	struct tc3_args *a = arg;

	set_fifo(50);

	/* Acquire mtx1 first, so A will block on it */
	pthread_mutex_lock(a->mtx1);
	a->b_has_mtx1 = 1;

	/* Wait until C holds mtx2 before blocking on it */
	while (!a->c_has_mtx2)
		sched_yield();

	/* Now block on mtx2 - chain: A->mtx1->B->mtx2->C */
	pthread_mutex_lock(a->mtx2);
	pthread_mutex_unlock(a->mtx2);

	pthread_mutex_unlock(a->mtx1);
	return NULL;
}

static void *tc3_a_thread(void *arg)
{
	struct tc3_args *a = arg;

	set_fifo(80);

	/* Wait until the full chain is established */
	while (!a->b_has_mtx1 || !a->c_has_mtx2)
		sched_yield();

	pthread_mutex_lock(a->mtx1);
	pthread_mutex_unlock(a->mtx1);
	return NULL;
}

static void run_tc3(void)
{
	pthread_mutex_t mtx1 = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t mtx2 = PTHREAD_MUTEX_INITIALIZER;
	struct tc3_args args = {
		.mtx1 = &mtx1,
		.mtx2 = &mtx2,
		.b_has_mtx1 = 0,
		.c_has_mtx2 = 0,
	};
	pthread_t ta, tb, tc;
	long long threshold_ns = (long long)TC3_CPU_THRESHOLD_MS * 1000000LL;

	/* Start C first so it grabs mtx2 */
	pthread_create(&tc, NULL, tc3_c_thread, &args);

	/* Wait for C to hold mtx2 */
	while (!args.c_has_mtx2)
		sched_yield();

	/* Start B - it will grab mtx1 then block on mtx2 */
	pthread_create(&tb, NULL, tc3_b_thread, &args);

	/* Wait for B to hold mtx1 */
	while (!args.b_has_mtx1)
		sched_yield();

	/* Start A - highest priority, blocks on mtx1 */
	pthread_create(&ta, NULL, tc3_a_thread, &args);

	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
	pthread_join(tc, NULL);

	pthread_mutex_destroy(&mtx1);
	pthread_mutex_destroy(&mtx2);

	if (args.c_cpu_during_hold_ns >= threshold_ns) {
		ksft_test_result_pass(
			"TC-3: PE traversed 2-level chain, C got cpu_hold=%lld ms >= %d ms\n",
			args.c_cpu_during_hold_ns / 1000000,
			TC3_CPU_THRESHOLD_MS);
	} else {
		ksft_test_result_fail(
			"TC-3: C did not get enough CPU (chain traversal failed?): %lld ms < %d ms\n",
			args.c_cpu_during_hold_ns / 1000000,
			TC3_CPU_THRESHOLD_MS);
	}
}

/* ---------- main ---------------------------------------------------------- */

int main(void)
{
	ksft_print_header();

#ifndef CONFIG_SCHED_PROXY_EXEC
	ksft_exit_skip("CONFIG_SCHED_PROXY_EXEC not enabled\n");
#endif

	if (getuid() != 0)
		ksft_exit_skip("requires root (needed for SCHED_FIFO)\n");

	if (!is_proxy_exec_enabled())
		ksft_exit_skip("sched_proxy_exec=0 on kernel cmdline, PE disabled\n");

	ksft_set_plan(3);

	run_tc1();
	run_tc2();
	run_tc3();

	ksft_finished();
}
