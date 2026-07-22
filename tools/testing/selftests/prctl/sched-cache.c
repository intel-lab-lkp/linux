// SPDX-License-Identifier: GPL-2.0
/*
 * Tests for prctl(PR_SCHED_CACHE): per-process (per-mm) control of
 * cache aware scheduling.
 *
 * The prctl stores attributes on the calling process's mm, so values
 * are shared by all threads, always inherited over fork(), and
 * inherited over execve() according to the per-thread
 * PR_SCHED_CACHE_INHERIT mask.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest_harness.h"

#ifndef PR_SCHED_CACHE
#define PR_SCHED_CACHE				82
# define PR_SCHED_CACHE_GET			1
# define PR_SCHED_CACHE_SET			2
# define PR_SCHED_CACHE_ENABLE			1
# define PR_SCHED_CACHE_AGGR_TOLERANCE_NR	2
# define PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE	3
# define PR_SCHED_CACHE_OVERAGGR_PCT		4
# define PR_SCHED_CACHE_INHERIT			5
# define PR_SCHED_CACHE_DEFAULT			(-1)
# define PR_SCHED_CACHE_INHERIT_ENABLE			(1UL << 0)
# define PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_NR	(1UL << 1)
# define PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_SIZE	(1UL << 2)
# define PR_SCHED_CACHE_INHERIT_OVERAGGR_PCT		(1UL << 3)
# define PR_SCHED_CACHE_INHERIT_MASK			\
	(PR_SCHED_CACHE_INHERIT_ENABLE |		\
	 PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_NR |	\
	 PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_SIZE |	\
	 PR_SCHED_CACHE_INHERIT_OVERAGGR_PCT)
#endif

static int sc_get(unsigned long attr, int *val)
{
	if (prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_GET, attr, val, 0))
		return -errno;
	return 0;
}

static int sc_set(unsigned long attr, unsigned long val)
{
	if (prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_SET, attr, val, 0))
		return -errno;
	return 0;
}

static int sc_supported(void)
{
	int val;

	return sc_get(PR_SCHED_CACHE_ENABLE, &val) == 0;
}

static void sc_reset_all(void)
{
	sc_set(PR_SCHED_CACHE_ENABLE, PR_SCHED_CACHE_DEFAULT);
	sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, PR_SCHED_CACHE_DEFAULT);
	sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, PR_SCHED_CACHE_DEFAULT);
	sc_set(PR_SCHED_CACHE_OVERAGGR_PCT, PR_SCHED_CACHE_DEFAULT);
	sc_set(PR_SCHED_CACHE_INHERIT, 0);
}

TEST(get_set_roundtrip)
{
	int val;

	if (!sc_supported())
		SKIP(return, "PR_SCHED_CACHE not supported");

	sc_reset_all();

	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_ENABLE, &val));
	ASSERT_EQ(PR_SCHED_CACHE_DEFAULT, val);
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, &val));
	ASSERT_EQ(PR_SCHED_CACHE_DEFAULT, val);
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, &val));
	ASSERT_EQ(PR_SCHED_CACHE_DEFAULT, val);
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_OVERAGGR_PCT, &val));
	ASSERT_EQ(PR_SCHED_CACHE_DEFAULT, val);
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_INHERIT, &val));
	ASSERT_EQ(0, val);

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_ENABLE, 0));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_ENABLE, &val));
	ASSERT_EQ(0, val);
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_ENABLE, 1));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_ENABLE, &val));
	ASSERT_EQ(1, val);

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 7));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, &val));
	ASSERT_EQ(7, val);

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, 13));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, &val));
	ASSERT_EQ(13, val);

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_OVERAGGR_PCT, 155));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_OVERAGGR_PCT, &val));
	ASSERT_EQ(155, val);

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_INHERIT,
			    PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_NR));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_INHERIT, &val));
	ASSERT_EQ(PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_NR, (unsigned long)val);

	/* DEFAULT resets the inherit mask to empty */
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_INHERIT, PR_SCHED_CACHE_DEFAULT));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_INHERIT, &val));
	ASSERT_EQ(0, val);

	/* both the sign-extended and the 32-bit truncated DEFAULT reset */
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR,
			    PR_SCHED_CACHE_DEFAULT));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, &val));
	ASSERT_EQ(PR_SCHED_CACHE_DEFAULT, val);
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, 0xffffffffUL));
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, &val));
	ASSERT_EQ(PR_SCHED_CACHE_DEFAULT, val);

	sc_reset_all();
}

TEST(invalid_arguments)
{
	int val;

	if (!sc_supported())
		SKIP(return, "PR_SCHED_CACHE not supported");

	sc_reset_all();

	/* out of range values */
	ASSERT_EQ(-EINVAL, sc_set(PR_SCHED_CACHE_ENABLE, 2));
	ASSERT_EQ(-EINVAL, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 101));
	ASSERT_EQ(-EINVAL, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, 101));
	ASSERT_EQ(-EINVAL, sc_set(PR_SCHED_CACHE_OVERAGGR_PCT, 1001));
	ASSERT_EQ(-EINVAL, sc_set(PR_SCHED_CACHE_INHERIT,
				  PR_SCHED_CACHE_INHERIT_MASK + 1));

	/* unknown attribute */
	ASSERT_EQ(-EINVAL, sc_set(0, 1));
	ASSERT_EQ(-EINVAL, sc_set(PR_SCHED_CACHE_INHERIT + 1, 1));
	ASSERT_EQ(-EINVAL, sc_get(0, &val));
	ASSERT_EQ(-EINVAL, sc_get(PR_SCHED_CACHE_INHERIT + 1, &val));

	/* unknown op */
	errno = 0;
	ASSERT_EQ(-1, prctl(PR_SCHED_CACHE, 0, PR_SCHED_CACHE_ENABLE, 0, 0));
	ASSERT_EQ(EINVAL, errno);
	errno = 0;
	ASSERT_EQ(-1, prctl(PR_SCHED_CACHE, 3, PR_SCHED_CACHE_ENABLE, 0, 0));
	ASSERT_EQ(EINVAL, errno);

	/* nonzero unused argument */
	errno = 0;
	ASSERT_EQ(-1, prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_SET,
			    PR_SCHED_CACHE_ENABLE, 1, 1));
	ASSERT_EQ(EINVAL, errno);

	/* bad GET pointer */
	errno = 0;
	ASSERT_EQ(-1, prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_GET,
			    PR_SCHED_CACHE_ENABLE, NULL, 0));
	ASSERT_EQ(EFAULT, errno);

	/* values rejected with -EINVAL must not be stored */
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_ENABLE, &val));
	ASSERT_EQ(PR_SCHED_CACHE_DEFAULT, val);
}

struct thread_ctx {
	int nr_seen;
	int inherit_seen;
	int ret;
};

static void *thread_fn(void *arg)
{
	struct thread_ctx *ctx = arg;

	ctx->ret = sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, &ctx->nr_seen);
	if (!ctx->ret)
		ctx->ret = sc_get(PR_SCHED_CACHE_INHERIT, &ctx->inherit_seen);
	if (!ctx->ret)
		ctx->ret = sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 42);
	/* clearing this thread's mask must not affect the creator's */
	if (!ctx->ret)
		ctx->ret = sc_set(PR_SCHED_CACHE_INHERIT, 0);

	return NULL;
}

TEST(values_shared_by_threads_inherit_mask_is_not)
{
	struct thread_ctx ctx = { .nr_seen = -2, .inherit_seen = -2 };
	pthread_t thread;
	int val;

	if (!sc_supported())
		SKIP(return, "PR_SCHED_CACHE not supported");

	sc_reset_all();

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 31));
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_INHERIT,
			    PR_SCHED_CACHE_INHERIT_ENABLE));

	ASSERT_EQ(0, pthread_create(&thread, NULL, thread_fn, &ctx));
	ASSERT_EQ(0, pthread_join(thread, NULL));
	ASSERT_EQ(0, ctx.ret);

	/* attribute values live on the shared mm */
	ASSERT_EQ(31, ctx.nr_seen);
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, &val));
	ASSERT_EQ(42, val);

	/*
	 * The inherit mask is per thread: the new thread starts with a
	 * copy of its creator's mask, and clearing it in the thread
	 * does not touch the creator's.
	 */
	ASSERT_EQ(PR_SCHED_CACHE_INHERIT_ENABLE,
		  (unsigned long)ctx.inherit_seen);
	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_INHERIT, &val));
	ASSERT_EQ(PR_SCHED_CACHE_INHERIT_ENABLE, (unsigned long)val);

	sc_reset_all();
}

TEST(fork_always_inherits)
{
	pid_t pid;
	int status;
	int val;

	if (!sc_supported())
		SKIP(return, "PR_SCHED_CACHE not supported");

	sc_reset_all();

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_ENABLE, 1));
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 9));
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, 11));
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_OVERAGGR_PCT, 77));

	pid = fork();
	ASSERT_LE(0, pid);
	if (pid == 0) {
		int val;

		if (sc_get(PR_SCHED_CACHE_ENABLE, &val) || val != 1)
			_exit(1);
		if (sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, &val) || val != 9)
			_exit(2);
		if (sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, &val) || val != 11)
			_exit(3);
		if (sc_get(PR_SCHED_CACHE_OVERAGGR_PCT, &val) || val != 77)
			_exit(4);

		/* changes in the child must not leak back to the parent */
		if (sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 50))
			_exit(5);

		_exit(0);
	}

	ASSERT_EQ(pid, waitpid(pid, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));

	ASSERT_EQ(0, sc_get(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, &val));
	ASSERT_EQ(9, val);

	sc_reset_all();
}

static int exec_check(int expect_enable, int expect_nr,
		      int expect_size, int expect_pct)
{
	char enable[16], nr[16], size[16], pct[16];
	pid_t pid;
	int status;

	snprintf(enable, sizeof(enable), "%d", expect_enable);
	snprintf(nr, sizeof(nr), "%d", expect_nr);
	snprintf(size, sizeof(size), "%d", expect_size);
	snprintf(pct, sizeof(pct), "%d", expect_pct);

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		setenv("SCHED_CACHE_EXEC_MODE", "1", 1);
		setenv("SCHED_CACHE_EXPECT_ENABLE", enable, 1);
		setenv("SCHED_CACHE_EXPECT_NR", nr, 1);
		setenv("SCHED_CACHE_EXPECT_SIZE", size, 1);
		setenv("SCHED_CACHE_EXPECT_PCT", pct, 1);
		execl("/proc/self/exe", "sched-cache", NULL);
		_exit(126);
	}

	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status))
		return -1;

	return WEXITSTATUS(status);
}

TEST(execve_inheritance)
{
	if (!sc_supported())
		SKIP(return, "PR_SCHED_CACHE not supported");

	sc_reset_all();

	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_ENABLE, 1));
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 21));
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE, 22));
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_OVERAGGR_PCT, 23));

	/* without inherit flags execve() resets everything */
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_INHERIT, 0));
	ASSERT_EQ(0, exec_check(PR_SCHED_CACHE_DEFAULT,
				PR_SCHED_CACHE_DEFAULT,
				PR_SCHED_CACHE_DEFAULT,
				PR_SCHED_CACHE_DEFAULT));

	/* selected attributes survive execve() */
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_INHERIT,
			    PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_NR |
			    PR_SCHED_CACHE_INHERIT_OVERAGGR_PCT));
	ASSERT_EQ(0, exec_check(PR_SCHED_CACHE_DEFAULT, 21,
				PR_SCHED_CACHE_DEFAULT, 23));

	/* all of them */
	ASSERT_EQ(0, sc_set(PR_SCHED_CACHE_INHERIT,
			    PR_SCHED_CACHE_INHERIT_MASK));
	ASSERT_EQ(0, exec_check(1, 21, 22, 23));

	sc_reset_all();
}

static int exec_expect(const char *env, unsigned long attr)
{
	const char *str = getenv(env);
	int val;

	if (!str)
		return 1;
	if (sc_get(attr, &val))
		return 1;
	if (val != atoi(str))
		return 1;

	return 0;
}

static int exec_check_main(void)
{
	int bad = 0;

	bad |= exec_expect("SCHED_CACHE_EXPECT_ENABLE", PR_SCHED_CACHE_ENABLE);
	bad |= exec_expect("SCHED_CACHE_EXPECT_NR",
			   PR_SCHED_CACHE_AGGR_TOLERANCE_NR);
	bad |= exec_expect("SCHED_CACHE_EXPECT_SIZE",
			   PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE);
	bad |= exec_expect("SCHED_CACHE_EXPECT_PCT",
			   PR_SCHED_CACHE_OVERAGGR_PCT);

	return bad;
}

int main(int argc, char **argv)
{
	if (getenv("SCHED_CACHE_EXEC_MODE"))
		return exec_check_main();

	return test_harness_run(argc, argv);
}
