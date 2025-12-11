// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright Collabora Ltd., 2021
 *
 * futex cmp requeue test by André Almeida <andrealmeid@collabora.com>
 */

#include <pthread.h>
#include <limits.h>
#include <linux/compiler.h>

#include "futextest.h"
#include "../../kselftest_harness.h"

#define timeout_ns  30000000
#define WAKE_WAIT_US 10000

volatile futex_t *f1;
static pthread_barrier_t barrier;

void *waiterfn(void *arg)
{
	struct timespec to;

	to.tv_sec = 0;
	to.tv_nsec = timeout_ns;

	WRITE_ONCE(*((pid_t *)arg), gettid());
	pthread_barrier_wait(&barrier);

	if (futex_wait(f1, *f1, &to, 0))
		printf("waiter failed errno %d\n", errno);

	return NULL;
}

static int get_thread_state(pid_t pid)
{
	FILE *fp;
	char buf[80], tag[80];
	char val = 0;

	snprintf(buf, sizeof(buf), "/proc/%d/status", pid);
	fp = fopen(buf, "r");
	if (!fp)
		return -1;

	while (fgets(buf, sizeof(buf), fp))
		if (fscanf(fp, "%s %c\n", tag, &val) == 2 && !strcmp(tag, "State:"))
			break;

	fclose(fp);
	return val;
}

TEST(requeue_single)
{
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	pthread_t waiter;
	pid_t tids;
	int res, state;

	f1 = &_f1;
	pthread_barrier_init(&barrier, NULL, 2);

	/*
	 * Requeue a waiter from f1 to f2, and wake f2.
	 */
	if (pthread_create(&waiter, NULL, waiterfn, &tids))
		ksft_exit_fail_msg("pthread_create failed\n");

	pthread_barrier_wait(&barrier);
	pthread_barrier_destroy(&barrier);
	while ((state = get_thread_state(READ_ONCE(tids))) != 'S') {
		usleep(WAKE_WAIT_US);

		if (state < 0)
			break;
	}

	ksft_print_dbg_msg("Requeuing 1 futex from f1 to f2\n");
	res = futex_cmp_requeue(f1, 0, &f2, 0, 1, 0);
	if (res != 1)
		ksft_test_result_fail("futex_requeue simple returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");

	ksft_print_dbg_msg("Waking 1 futex at f2\n");
	res = futex_wake(&f2, 1, 0);
	if (res != 1) {
		ksft_test_result_fail("futex_requeue simple returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
	} else {
		ksft_test_result_pass("futex_requeue simple succeeds\n");
	}

	pthread_join(waiter, NULL);
}

TEST(requeue_multiple)
{
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	pthread_t waiter[10];
	pid_t tids[10];
	int res, i, state;

	f1 = &_f1;

	/*
	 * Create 10 waiters at f1. At futex_requeue, wake 3 and requeue 7.
	 * At futex_wake, wake INT_MAX (should be exactly 7).
	 */
	for (i = 0; i < 10; i++) {
		pthread_barrier_init(&barrier, NULL, 2);

		if (pthread_create(&waiter[i], NULL, waiterfn, &tids[i]))
			ksft_exit_fail_msg("pthread_create failed\n");

		pthread_barrier_wait(&barrier);
		pthread_barrier_destroy(&barrier);

		while ((state = get_thread_state(READ_ONCE(tids[i]))) != 'S') {
			usleep(WAKE_WAIT_US);

			if (state < 0)
				break;
		}
	}

	ksft_print_dbg_msg("Waking 3 futexes at f1 and requeuing 7 futexes from f1 to f2\n");
	res = futex_cmp_requeue(f1, 0, &f2, 3, 7, 0);
	if (res != 10) {
		ksft_test_result_fail("futex_requeue many returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
	}

	ksft_print_dbg_msg("Waking INT_MAX futexes at f2\n");
	res = futex_wake(&f2, INT_MAX, 0);
	if (res != 7) {
		ksft_test_result_fail("futex_requeue many returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
	} else {
		ksft_test_result_pass("futex_requeue many succeeds\n");
	}

	for (i = 0; i < 10; i++)
		pthread_join(waiter[i], NULL);
}

TEST_HARNESS_MAIN
