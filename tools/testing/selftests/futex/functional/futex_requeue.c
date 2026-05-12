// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright Collabora Ltd., 2021
 *
 * futex cmp requeue test by André Almeida <andrealmeid@collabora.com>
 */

#include <pthread.h>
#include <limits.h>

#include "futextest.h"
#include "futex_thread.h"
#include "kselftest_harness.h"

#define FUTEX_WAIT_TIMEOUT_SECS 1
#define WAIT_THREAD_CREATE_TIMEOUT_USECS (USEC_PER_SEC / 2) /* 500ms */

volatile futex_t *f1;

static int waiterfn(void *arg)
{
	struct timespec to;

	to.tv_sec = FUTEX_WAIT_TIMEOUT_SECS;
	to.tv_nsec = 0;

	if (futex_wait(f1, *f1, &to, 0)) {
		printf("waiter failed errno %d\n", errno);
		return -errno;
	}

	return 0;
}

TEST(requeue_single)
{
	struct futex_thread waiter;
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	int ret;

	f1 = &_f1;

	/*
	 * Requeue a waiter from f1 to f2, and wake f2.
	 */
	ASSERT_EQ(0, futex_thread_create(&waiter, waiterfn, NULL));

	ret = futex_wait_for_thread(&waiter, WAIT_THREAD_CREATE_TIMEOUT_USECS);
	if (ret < 0)
		usleep(WAIT_THREAD_CREATE_TIMEOUT_USECS);

	EXPECT_EQ(1, futex_cmp_requeue(f1, 0, &f2, 0, 1, 0));
	EXPECT_EQ(1, futex_wake(&f2, 1, 0));

	EXPECT_EQ(0, futex_thread_destroy(&waiter));
}

TEST(requeue_multiple)
{
	struct futex_thread waiter[10];
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	int i, ret;

	f1 = &_f1;

	/*
	 * Create 10 waiters at f1. At futex_requeue, wake 3 and requeue 7.
	 * At futex_wake, wake INT_MAX (should be exactly 7).
	 */
	for (i = 0; i < 10; i++)
		ASSERT_EQ(0, futex_thread_create(&waiter[i], waiterfn, NULL));

	for (i = 0; i < 10; i++) {
		ret = futex_wait_for_thread(&waiter[i], WAIT_THREAD_CREATE_TIMEOUT_USECS / 10);
		if (ret < 0) {
			/* /proc not available, give all threads time to enter futex wait */
			usleep(WAIT_THREAD_CREATE_TIMEOUT_USECS);
			break;
		}
	}

	EXPECT_EQ(10, futex_cmp_requeue(f1, 0, &f2, 3, 7, 0));
	EXPECT_EQ(7, futex_wake(&f2, INT_MAX, 0));

	for (i = 0; i < 10; i++)
		EXPECT_EQ(0, futex_thread_destroy(&waiter[i]));
}

TEST_HARNESS_MAIN
