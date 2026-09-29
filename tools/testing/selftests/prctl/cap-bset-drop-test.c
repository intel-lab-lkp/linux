// SPDX-License-Identifier: GPL-2.0
/*
 * Tests for PR_CAPBSET_DROP_MASK: argument validation, permission checking,
 * and process-wide application of the bounding set drop to sibling threads
 * and to threads created afterwards.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/capability.h>

#include "../kselftest.h"

#ifndef PR_CAPBSET_DROP_MASK
#define PR_CAPBSET_DROP_MASK 82
#endif

#define N_THREADS 4

#define CHILD_PASS	0
#define CHILD_FAIL	1
#define CHILD_SKIP	2

static int pipe_fd[2];
static atomic_int threads_ready;
static atomic_int threads_ok;
static unsigned long dropped_cap;

static atomic_int fork_go;
static atomic_int fork_ok;

static atomic_int conc_stop;
static atomic_int conc_dropped;
static atomic_int conc_bad;

#define MULTI_DROP 8
static int multi_caps[MULTI_DROP];
static int multi_n;
static pthread_barrier_t multi_start;
static atomic_int multi_done;
static atomic_int multi_bad;

static int last_cap;

static int read_last_cap(void)
{
	char buf[32];
	int fd, n, v = -1;

	fd = open("/proc/sys/kernel/cap_last_cap", O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	v = atoi(buf);
	return v;
}

static int bset_has(unsigned long cap)
{
	return prctl(PR_CAPBSET_READ, cap, 0, 0, 0) > 0;
}

static int drop_cap(unsigned long cap)
{
	unsigned long low = cap < 32 ? 1UL << cap : 0;
	unsigned long high = cap < 32 ? 0 : 1UL << (cap - 32);

	return prctl(PR_CAPBSET_DROP_MASK, low, high, 0, 0);
}

static int run_child(int (*fn)(void))
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return CHILD_FAIL;
	if (pid == 0)
		_exit(fn());
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
		return CHILD_FAIL;
	return WEXITSTATUS(status);
}

static void report_child(int ret, const char *name)
{
	if (ret == CHILD_PASS)
		ksft_test_result_pass("%s\n", name);
	else if (ret == CHILD_SKIP)
		ksft_test_result_skip("%s\n", name);
	else
		ksft_test_result_fail("%s\n", name);
}

static void run_test(int cond, int (*fn)(void), const char *name)
{
	if (!cond) {
		ksft_test_result_skip("%s\n", name);
		return;
	}
	report_child(run_child(fn), name);
}

/*
 * Return the first capability in [lo, hi] that is set in the bounding set,
 * or -1 if there is none.
 */
static int pick_cap(int lo, int hi)
{
	int cap;

	for (cap = lo; cap <= hi; cap++)
		if (bset_has(cap))
			return cap;
	return -1;
}

static int have_cap_setpcap(void)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[2];

	if (syscall(SYS_capget, &hdr, data))
		return 0;
	return !!(data[0].effective & (1U << CAP_SETPCAP));
}

static int drop_effective_cap_setpcap(void)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[2];

	if (syscall(SYS_capget, &hdr, data))
		return -1;
	data[0].effective &= ~(1U << CAP_SETPCAP);
	return syscall(SYS_capset, &hdr, data);
}

static void *blocked_worker(void *arg)
{
	char c;

	(void)arg;

	atomic_fetch_add(&threads_ready, 1);

	/*
	 * Block until the main thread has issued the drop.  The drop is made
	 * effective for the whole group through the thread group's pending
	 * mask, so it must be visible here once we run again.
	 */
	for (;;) {
		ssize_t n = read(pipe_fd[0], &c, 1);

		if (n == 1)
			break;
		if (n < 0 && errno == EINTR)
			continue;
		return NULL;
	}

	if (prctl(PR_CAPBSET_READ, dropped_cap, 0, 0, 0) == 0)
		atomic_fetch_add(&threads_ok, 1);
	return NULL;
}

static void *late_worker(void *arg)
{
	(void)arg;

	/* Must inherit the reduced bounding set of the parent thread. */
	if (prctl(PR_CAPBSET_READ, dropped_cap, 0, 0, 0) == 0)
		atomic_fetch_add(&threads_ok, 1);
	return NULL;
}

/*
 * Child for the functional tests: drop @cap with PR_CAPBSET_DROP_MASK and
 * verify that the calling thread, all blocked sibling threads and a thread
 * created afterwards lose it from their bounding sets.
 */
static int drop_test_child(void)
{
	unsigned long cap = dropped_cap;
	pthread_t t[N_THREADS], late;
	char c = 'x';
	int i, ret;

	atomic_store(&threads_ready, 0);
	atomic_store(&threads_ok, 0);

	if (pipe(pipe_fd))
		return CHILD_FAIL;

	for (i = 0; i < N_THREADS; i++) {
		if (pthread_create(&t[i], NULL, blocked_worker, NULL)) {
			close(pipe_fd[0]);
			close(pipe_fd[1]);
			return CHILD_FAIL;
		}
	}
	while (atomic_load(&threads_ready) < N_THREADS)
		sched_yield();

	ret = drop_cap(cap);
	if (ret) {
		ksft_print_msg("PR_CAPBSET_DROP_MASK(cap %lu) failed: %s\n",
			       cap, strerror(errno));
		ret = CHILD_FAIL;
		goto out;
	}

	for (i = 0; i < N_THREADS; i++) {
		if (write(pipe_fd[1], &c, 1) != 1) {
			ksft_print_msg("pipe write failed: %s\n",
				       strerror(errno));
			/* Close the write end so the workers see EOF. */
			close(pipe_fd[1]);
			pipe_fd[1] = -1;
			ret = CHILD_FAIL;
			goto out_join;
		}
	}
out_join:
	for (i = 0; i < N_THREADS; i++)
		pthread_join(t[i], NULL);

	if (atomic_load(&threads_ok) != N_THREADS) {
		ksft_print_msg("only %d of %d sibling threads saw the drop\n",
			       atomic_load(&threads_ok), N_THREADS);
		ret = CHILD_FAIL;
		goto out;
	}

	/* The calling thread must have dropped it synchronously. */
	if (bset_has(cap)) {
		ksft_print_msg("calling thread still has capability %lu\n",
			       cap);
		ret = CHILD_FAIL;
		goto out;
	}

	/* A thread created afterwards must inherit the reduced set. */
	if (pthread_create(&late, NULL, late_worker, NULL))
		goto out_ok;
	pthread_join(late, NULL);
	if (atomic_load(&threads_ok) != N_THREADS + 1) {
		ksft_print_msg("late thread did not inherit the drop\n");
		ret = CHILD_FAIL;
		goto out;
	}

out_ok:
	ret = CHILD_PASS;
out:
	if (pipe_fd[1] >= 0)
		close(pipe_fd[1]);
	close(pipe_fd[0]);
	return ret;
}

/*
 * A sibling thread that did not call PR_CAPBSET_DROP_MASK has not had its own
 * cred updated, but it must still observe the drop and must pass the reduced
 * bounding set to any child it forks.
 */
static void *fork_sibling(void *arg)
{
	pid_t pid;
	int status;

	(void)arg;

	while (!atomic_load(&fork_go))
		sched_yield();

	if (bset_has(dropped_cap))
		atomic_store(&fork_ok, 0);

	pid = fork();
	if (pid == 0)
		_exit(bset_has(dropped_cap) ? 1 : 0);
	if (pid < 0) {
		atomic_store(&fork_ok, 0);
		return NULL;
	}
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		atomic_store(&fork_ok, 0);
	return NULL;
}

static int fork_test_child(void)
{
	pthread_t sib;

	atomic_store(&fork_go, 0);
	atomic_store(&fork_ok, 1);

	if (pthread_create(&sib, NULL, fork_sibling, NULL))
		return CHILD_FAIL;

	if (drop_cap(dropped_cap)) {
		atomic_store(&fork_go, 1);
		pthread_join(sib, NULL);
		return CHILD_FAIL;
	}
	atomic_store(&fork_go, 1);
	pthread_join(sib, NULL);

	return atomic_load(&fork_ok) ? CHILD_PASS : CHILD_FAIL;
}

/*
 * Threads created while the drop is in flight (or immediately after) must all
 * observe it: the pending mask is shared by the whole group, so a thread can
 * never be born with a capability that a concurrent drop removed.
 */
static void *conc_worker(void *arg)
{
	(void)arg;

	while (!atomic_load(&conc_dropped))
		sched_yield();
	if (bset_has(dropped_cap))
		atomic_fetch_add(&conc_bad, 1);
	return NULL;
}

static void *conc_spawner(void *arg)
{
	(void)arg;

	while (!atomic_load(&conc_stop)) {
		pthread_t t;

		if (pthread_create(&t, NULL, conc_worker, NULL) == 0)
			pthread_join(t, NULL);
	}
	return NULL;
}

static int conc_test_child(void)
{
	pthread_t sp[4];
	int i;

	atomic_store(&conc_stop, 0);
	atomic_store(&conc_dropped, 0);
	atomic_store(&conc_bad, 0);

	for (i = 0; i < 4; i++) {
		if (pthread_create(&sp[i], NULL, conc_spawner, NULL))
			return CHILD_FAIL;
	}

	if (drop_cap(dropped_cap)) {
		atomic_store(&conc_stop, 1);
		for (i = 0; i < 4; i++)
			pthread_join(sp[i], NULL);
		return CHILD_FAIL;
	}
	atomic_store(&conc_dropped, 1);
	usleep(20000);
	atomic_store(&conc_stop, 1);
	for (i = 0; i < 4; i++)
		pthread_join(sp[i], NULL);

	return atomic_load(&conc_bad) ? CHILD_FAIL : CHILD_PASS;
}

/*
 * Several threads invoke PR_CAPBSET_DROP_MASK concurrently with different
 * masks while other threads are cloning.  The primitive is drop-only, so
 * concurrent calls commute and the result is always the union of the requested
 * drops, regardless of who "wins"; every thread, including ones created during
 * the race, must end up without any of them.
 */
static int multi_bset_has_any(void)
{
	int i;

	for (i = 0; i < multi_n; i++)
		if (bset_has(multi_caps[i]))
			return 1;
	return 0;
}

static void *multi_reader(void *arg)
{
	(void)arg;

	while (!atomic_load(&multi_done))
		sched_yield();
	if (multi_bset_has_any())
		atomic_fetch_add(&multi_bad, 1);
	return NULL;
}

static void *multi_spawner(void *arg)
{
	(void)arg;

	while (!atomic_load(&multi_done)) {
		pthread_t t;

		if (pthread_create(&t, NULL, multi_reader, NULL) == 0)
			pthread_join(t, NULL);
	}
	return NULL;
}

static void *multi_dropper(void *arg)
{
	long i = (long)arg;

	pthread_barrier_wait(&multi_start);
	/* Drop its own cap, then immediately race a second time. */
	drop_cap(multi_caps[i]);
	drop_cap(multi_caps[(i + 1) % multi_n]);
	return NULL;
}

static int multi_drop_test_child(void)
{
	pthread_t dr[MULTI_DROP], sp[4], late;
	int i;

	multi_n = 0;
	for (i = 0; i <= last_cap && multi_n < MULTI_DROP; i++) {
		if (i == CAP_SETPCAP)
			continue;
		if (bset_has(i))
			multi_caps[multi_n++] = i;
	}
	if (multi_n < 2)
		return CHILD_SKIP;

	atomic_store(&multi_done, 0);
	atomic_store(&multi_bad, 0);
	pthread_barrier_init(&multi_start, NULL, multi_n + 1);

	for (i = 0; i < 4; i++) {
		if (pthread_create(&sp[i], NULL, multi_spawner, NULL))
			return CHILD_FAIL;
	}
	for (i = 0; i < multi_n; i++) {
		if (pthread_create(&dr[i], NULL, multi_dropper,
				   (void *)(long)i))
			return CHILD_FAIL;
	}
	pthread_barrier_wait(&multi_start);
	for (i = 0; i < multi_n; i++)
		pthread_join(dr[i], NULL);
	atomic_store(&multi_done, 1);
	for (i = 0; i < 4; i++)
		pthread_join(sp[i], NULL);

	if (multi_bset_has_any()) /* the calling thread itself */
		return CHILD_FAIL;
	if (atomic_load(&multi_bad))
		return CHILD_FAIL;

	pthread_create(&late, NULL, multi_reader, NULL);
	pthread_join(late, NULL);
	if (atomic_load(&multi_bad))
		return CHILD_FAIL;

	return CHILD_PASS;
}

/*
 * With CAP_SETPCAP dropped from the effective set, a non-empty mask must be
 * rejected with EPERM.
 */
static int eperm_child(void)
{
	if (drop_effective_cap_setpcap()) {
		ksft_print_msg("capset failed: %s\n", strerror(errno));
		return CHILD_FAIL;
	}

	if (prctl(PR_CAPBSET_DROP_MASK, 1, 0, 0, 0) == 0 ||
	    errno != EPERM) {
		ksft_print_msg("PR_CAPBSET_DROP_MASK without CAP_SETPCAP: %s\n",
			       strerror(errno));
		return CHILD_FAIL;
	}
	return CHILD_PASS;
}

/*
 * An empty mask is a no-op and must succeed even without CAP_SETPCAP: the
 * "nothing to drop" check precedes the permission check.
 */
static int empty_child(void)
{
	if (drop_effective_cap_setpcap()) {
		ksft_print_msg("capset failed: %s\n", strerror(errno));
		return CHILD_FAIL;
	}

	if (prctl(PR_CAPBSET_DROP_MASK, 0, 0, 0, 0) != 0) {
		ksft_print_msg("empty mask without CAP_SETPCAP: %s\n",
			       strerror(errno));
		return CHILD_FAIL;
	}
	return CHILD_PASS;
}

int main(void)
{
	int privileged = have_cap_setpcap();
	int cap_lo, cap_hi;
	unsigned long long unknown;

	last_cap = read_last_cap();

	ksft_print_header();
	ksft_set_plan(10);

	/* Argument validation, independent of privileges. */
	if (prctl(PR_CAPBSET_DROP_MASK, 0, 0, 1, 0) == 0 ||
	    errno != EINVAL)
		ksft_test_result_fail("PR_CAPBSET_DROP_MASK nonzero arg4\n");
	else
		ksft_test_result_pass("PR_CAPBSET_DROP_MASK nonzero arg4\n");

	if (prctl(PR_CAPBSET_DROP_MASK, 0, 0, 0, 1) == 0 ||
	    errno != EINVAL)
		ksft_test_result_fail("PR_CAPBSET_DROP_MASK nonzero arg5\n");
	else
		ksft_test_result_pass("PR_CAPBSET_DROP_MASK nonzero arg5\n");

	/*
	 * An empty mask is a no-op and succeeds even without CAP_SETPCAP,
	 * because the emptiness check comes before the permission check.
	 */
	if (privileged) {
		ksft_test_result(run_child(empty_child) == CHILD_PASS,
				 "empty mask without CAP_SETPCAP\n");
	} else if (prctl(PR_CAPBSET_DROP_MASK, 0, 0, 0, 0) != 0) {
		ksft_test_result_fail("empty mask without CAP_SETPCAP (errno=%d, old kernel?)\n",
				      errno);
	} else {
		ksft_test_result_pass("empty mask without CAP_SETPCAP\n");
	}

	/* A non-empty mask without CAP_SETPCAP must fail with EPERM. */
	if (privileged) {
		ksft_test_result(run_child(eperm_child) == CHILD_PASS,
				 "EPERM without CAP_SETPCAP\n");
	} else if (prctl(PR_CAPBSET_DROP_MASK, 1, 0, 0, 0) == 0 ||
		   errno != EPERM) {
		ksft_test_result_fail("EPERM without CAP_SETPCAP (errno=%d, old kernel?)\n",
				      errno);
	} else {
		ksft_test_result_pass("EPERM without CAP_SETPCAP\n");
	}

	/* Bits for capabilities unknown to the kernel are silently ignored. */
	unknown = (last_cap >= 0 && last_cap < 63) ? (~0ULL << (last_cap + 1)) : 0;
	if (unknown) {
		unsigned long low = (unsigned int)unknown;
		unsigned long high = (unsigned int)(unknown >> 32);

		if (!privileged) {
			ksft_test_result_skip("unknown capabilities ignored (needs CAP_SETPCAP)\n");
		} else if (prctl(PR_CAPBSET_DROP_MASK, low, high, 0, 0)) {
			ksft_test_result_fail("unknown capabilities ignored (errno=%d)\n",
					      errno);
		} else {
			ksft_test_result_pass("unknown capabilities ignored\n");
		}
	} else {
		ksft_test_result_skip("unknown capabilities ignored\n");
	}

	/*
	 * Functional tests, each in a child so the parent keeps its own
	 * bounding set.  A low-word and a high-word capability cover the
	 * arg2/arg3 split.
	 */
	cap_lo = last_cap >= 0 ? pick_cap(0, last_cap < 31 ? last_cap : 31) : -1;
	cap_hi = last_cap > 31 ? pick_cap(32, last_cap) : -1;

	dropped_cap = cap_lo;
	run_test(privileged && cap_lo >= 0, drop_test_child,
		 "PR_CAPBSET_DROP_MASK drops all threads");

	dropped_cap = cap_hi;
	run_test(privileged && cap_hi >= 0, drop_test_child,
		 "PR_CAPBSET_DROP_MASK high word");

	dropped_cap = cap_lo;

	/*
	 * A child forked by a sibling thread that has not materialized the
	 * drop into its own cred must still inherit the reduced bounding set.
	 */
	run_test(privileged && cap_lo >= 0, fork_test_child,
		 "PR_CAPBSET_DROP_MASK inherited by forked child");

	/*
	 * Threads created concurrently with the drop must all observe it.
	 */
	run_test(privileged && cap_lo >= 0, conc_test_child,
		 "PR_CAPBSET_DROP_MASK concurrent threads");

	/*
	 * Concurrent callers with different masks, racing with thread
	 * creation: the drop-only primitive must commute and the union of all
	 * requested drops must be enforced on every thread.
	 */
	run_test(privileged, multi_drop_test_child,
		 "PR_CAPBSET_DROP_MASK concurrent masks");

	ksft_finished();
}
