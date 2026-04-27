// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright Collabora Ltd., 2021
 *
 * futex cmp requeue test by André Almeida <andrealmeid@collabora.com>
 */

#include <pthread.h>
#include <limits.h>

#include "futextest.h"
#include "kselftest_harness.h"

#define FUTEX_WAIT_TIMEOUT_S	3 /* 3s */
#define WAIT_THREAD_RETRIES	100
#define WAIT_THREAD_DELAY_US	(10000 * 100) /* 1s */

volatile futex_t *f1;

void *waiterfn(void *arg)
{
	struct timespec to;

	to.tv_sec = FUTEX_WAIT_TIMEOUT_S;
	to.tv_nsec = 0;

	if (futex_wait(f1, *f1, &to, 0))
		printf("waiter failed errno %d\n", errno);

	return NULL;
}

struct futex_thread {
	pthread_t thread;
	pthread_barrier_t barrier;
	pid_t tid;
	void *(*threadfn)(void *);
	void *arg;
};

static int wait_for_thread(FILE *fp, int timeout_us)
{
	char buf[80] = "";

	for (int i = 0; i < WAIT_THREAD_RETRIES; i++) {
		if (!fgets(buf, sizeof(buf), fp))
			return -EIO;
		if (!strncmp(buf, "futex", 5))
			return 0;
		usleep(timeout_us / WAIT_THREAD_RETRIES);
		rewind(fp);
	}
	return -ETIMEDOUT;
}

int futex_wait_for_thread(struct futex_thread *t, int timeout_us)
{
	char fname[80];
	FILE *fp;
	int res;

	snprintf(fname, sizeof(fname), "/proc/%d/wchan", t->tid);
	fp = fopen(fname, "r");
	if (!fp)
		return -EIO;
	res = wait_for_thread(fp, timeout_us);
	fclose(fp);
	return res;
}

static void *futex_thread_fn(void *arg)
{
	struct futex_thread *t = arg;

	t->tid = gettid();
	pthread_barrier_wait(&t->barrier);
	return t->threadfn(t->arg);
}

int futex_thread_create(struct futex_thread *t, void *(*threadfn)(void *), void *arg)
{
	int ret;

	pthread_barrier_init(&t->barrier, NULL, 2);
	t->tid = 0;
	t->threadfn = threadfn;
	t->arg = arg;

	ret = pthread_create(&t->thread, NULL, futex_thread_fn, t);
	if (ret)
		return ret;

	pthread_barrier_wait(&t->barrier);
	return 0;
}

TEST(requeue_single)
{
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	struct futex_thread waiter;

	f1 = &_f1;

	/*
	 * Requeue a waiter from f1 to f2, and wake f2.
	 */
	EXPECT_EQ(0, futex_thread_create(&waiter, waiterfn, NULL));
	futex_wait_for_thread(&waiter, WAIT_THREAD_DELAY_US);

	EXPECT_EQ(1, futex_cmp_requeue(f1, 0, &f2, 0, 1, 0));
	EXPECT_EQ(1, futex_wake(&f2, 1, 0));
}

TEST(requeue_multiple)
{
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	struct futex_thread waiter[10];

	f1 = &_f1;

	/*
	 * Create 10 waiters at f1. At futex_requeue, wake 3 and requeue 7.
	 * At futex_wake, wake INT_MAX (should be exactly 7).
	 */
	for (int i = 0; i < 10; i++) {
		EXPECT_EQ(0, futex_thread_create(&waiter[i], waiterfn, NULL));
		futex_wait_for_thread(&waiter[i], WAIT_THREAD_DELAY_US / 10);
	}

	EXPECT_EQ(10, futex_cmp_requeue(f1, 0, &f2, 3, 7, 0));
	EXPECT_EQ(7, futex_wake(&f2, INT_MAX, 0));
}

TEST_HARNESS_MAIN
