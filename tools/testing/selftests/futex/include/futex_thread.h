/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _FUTEX_THREAD_H
#define _FUTEX_THREAD_H
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#define USEC_PER_SEC 1000000L
#define WAIT_THREAD_RETRIES 100

struct futex_thread {
	pthread_t		thread;
	pthread_barrier_t	barrier;
	pid_t			tid;
	int			(*threadfn)(void *arg);
	void			*arg;
	int			retval;
};

static int __wait_for_thread(FILE *fp, int timeout_us)
{
	char buf[80] = "";
	int i;

	for (i = 0; i < WAIT_THREAD_RETRIES; i++) {
		if (!fgets(buf, sizeof(buf), fp))
			return -EIO;
		if (!strncmp(buf, "futex", 5))
			return 0;
		usleep(timeout_us / WAIT_THREAD_RETRIES);
		rewind(fp);
	}
	return -ETIMEDOUT;
}

static void *__futex_thread_fn(void *arg)
{
	struct futex_thread *t = arg;

	t->tid = gettid();
	pthread_barrier_wait(&t->barrier);
	t->retval = t->threadfn(t->arg);
	return NULL;
}

/**
 * futex_wait_for_thread - Wait for the child thread to sleep in the futex context
 * @t:          Thread handle.
 * @timeout_us: The timeout for waiting for the thread to enter the sleep state.
 */
static int futex_wait_for_thread(struct futex_thread *t, int timeout_us)
{
	char fname[80];
	FILE *fp;
	int res;

	snprintf(fname, sizeof(fname), "/proc/%d/wchan", t->tid);
	fp = fopen(fname, "r");
	if (!fp) {
		usleep(USEC_PER_SEC / 10); /* 100ms */
		return -EIO;
	}

	res = __wait_for_thread(fp, timeout_us);
	if (res == -EIO)
		usleep(USEC_PER_SEC / 10); /* 100ms */

	fclose(fp);
	return res;
}

/**
 * futex_thread_create - Create a new thread for testing.
 * @t:        The handle of the newly created thread.
 * @threadfn: The new thread starts execution by invoking threadfn
 * @arg:      The parameters passed to threadfn.
 */
static int futex_thread_create(struct futex_thread *t, int (*threadfn)(void *), void *arg)
{
	int ret;

	pthread_barrier_init(&t->barrier, NULL, 2);
	t->tid = 0;
	t->threadfn = threadfn;
	t->arg = arg;

	ret = pthread_create(&t->thread, NULL, __futex_thread_fn, t);
	if (ret)
		return ret;

	pthread_barrier_wait(&t->barrier);
	return 0;
}

/**
 * futex_thread_destroy - Wait for and reclaim the resources of the thread.
 * @t:      Thread handle.
 */
static int futex_thread_destroy(struct futex_thread *t)
{
	pthread_join(t->thread, NULL);
	pthread_barrier_destroy(&t->barrier);
	return t->retval;
}

#endif
