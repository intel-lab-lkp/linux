// SPDX-License-Identifier: GPL-2.0
/*
 * tlob_target.c - uprobe target binary for tlob selftests.
 *
 * Provides three start/stop probe pairs, each designed to exercise a
 * different dominant component of the detail_env_tlob ns breakdown:
 *
 *   tlob_busy_work    / tlob_busy_work_done    - busy-spin: running_ns dominates
 *   tlob_sleep_work   / tlob_sleep_work_done   - nanosleep: sleeping_ns dominates
 *   tlob_preempt_work / tlob_preempt_work_done - busy-spin + RT competitor:
 *                                                waiting_ns dominates
 *
 * Usage: tlob_target <duration_ms> [mode]
 *
 * mode is one of: busy (default), sleep, preempt.
 * Loops in 200 ms iterations until <duration_ms> has elapsed
 * (0 = run for ~24 hours).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef noinline
#define noinline __attribute__((noinline))
#endif

static inline int timespec_before(const struct timespec *a,
				   const struct timespec *b)
{
	return a->tv_sec < b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static void timespec_add_ms(struct timespec *ts, unsigned long ms)
{
	ts->tv_sec  += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* stop probe; noinline keeps the entry point visible to uprobes */
noinline void tlob_busy_work_done(void)
{
	/* empty: uprobe fires on entry */
}

/* start probe; busy-spin so running_ns dominates */
noinline void tlob_busy_work(unsigned long duration_ms)
{
	struct timespec start, now;
	unsigned long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (unsigned long)(now.tv_sec - start.tv_sec)
			  * 1000000000UL
			+ (unsigned long)(now.tv_nsec - start.tv_nsec);
	} while (elapsed < duration_ms * 1000000UL);

	tlob_busy_work_done();
}

/* stop probe; noinline keeps the entry point visible to uprobes */
noinline void tlob_sleep_work_done(void)
{
	/* empty: uprobe fires on entry */
}

/* start probe; nanosleep so sleeping_ns dominates */
noinline void tlob_sleep_work(unsigned long duration_ms)
{
	struct timespec ts = {
		.tv_sec  = duration_ms / 1000,
		.tv_nsec = (long)(duration_ms % 1000) * 1000000L,
	};
	nanosleep(&ts, NULL);
	tlob_sleep_work_done();
}

/* stop probe; noinline keeps the entry point visible to uprobes */
noinline void tlob_preempt_work_done(void)
{
	/* empty: uprobe fires on entry */
}

/*
 * start probe; busy-spin so an RT competitor on the same CPU drives
 * waiting_ns (prev_state==0 -> preempt event, task stays runnable off-CPU).
 */
noinline void tlob_preempt_work(unsigned long duration_ms)
{
	struct timespec start, now;
	unsigned long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (unsigned long)(now.tv_sec - start.tv_sec)
			  * 1000000000UL
			+ (unsigned long)(now.tv_nsec - start.tv_nsec);
	} while (elapsed < duration_ms * 1000000UL);

	tlob_preempt_work_done();
}

int main(int argc, char *argv[])
{
	unsigned long duration_ms = 0;
	const char *mode = "busy";
	struct timespec deadline, now;

	if (argc >= 2)
		duration_ms = strtoul(argv[1], NULL, 10);
	if (argc >= 3)
		mode = argv[2];

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	timespec_add_ms(&deadline, duration_ms ? duration_ms : 86400000UL);

	do {
		if (strcmp(mode, "sleep") == 0)
			tlob_sleep_work(200);
		else if (strcmp(mode, "preempt") == 0)
			tlob_preempt_work(200);
		else
			tlob_busy_work(200);
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (timespec_before(&now, &deadline));

	return 0;
}
