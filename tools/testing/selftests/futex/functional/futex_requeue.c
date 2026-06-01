// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright Collabora Ltd., 2021
 *
 * futex cmp requeue test by André Almeida <andrealmeid@collabora.com>
 */

#include <pthread.h>
#include <limits.h>
#include <string.h>

#include "futextest.h"
#include "kselftest_harness.h"

#define timeout_ns  30000000
#define WAKE_WAIT_US 10000

volatile futex_t *f1;

void *waiterfn(void *arg)
{
	struct __test_metadata *_metadata = (struct __test_metadata *)arg;
	struct timespec to;
	int res;

	to.tv_sec = 0;
	to.tv_nsec = timeout_ns;

	res = futex_wait(f1, *f1, &to, 0);
	if (res) {
		EXPECT_EQ(res, 0)
			TH_LOG("waiter failed errno %d: %s", errno, strerror(errno));
	}

	return NULL;
}

TEST(requeue_single)
{
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	pthread_t waiter[10];

	f1 = &_f1;

	/*
	 * Requeue a waiter from f1 to f2, and wake f2.
	 */
	ASSERT_EQ(pthread_create(&waiter[0], NULL, waiterfn, _metadata), 0)
		TH_LOG("pthread_create failed");

	usleep(WAKE_WAIT_US);

	EXPECT_EQ(futex_cmp_requeue(f1, 0, &f2, 0, 1, 0), 1);
	EXPECT_EQ(futex_wake(&f2, 1, 0), 1);

	pthread_join(waiter[0], NULL);
}

TEST(requeue_multiple)
{
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	pthread_t waiter[10];
	int i;

	f1 = &_f1;

	/*
	 * Create 10 waiters at f1. At futex_requeue, wake 3 and requeue 7.
	 * At futex_wake, wake INT_MAX (should be exactly 7).
	 */
	for (i = 0; i < 10; i++) {
		ASSERT_EQ(pthread_create(&waiter[i], NULL, waiterfn, _metadata), 0)
			TH_LOG("pthread_create failed for waiter %d", i);
	}

	usleep(WAKE_WAIT_US);

	EXPECT_EQ(futex_cmp_requeue(f1, 0, &f2, 3, 7, 0), 10);
	EXPECT_EQ(futex_wake(&f2, INT_MAX, 0), 7);

	for (i = 0; i < 10; i++)
		pthread_join(waiter[i], NULL);
}

TEST_HARNESS_MAIN
