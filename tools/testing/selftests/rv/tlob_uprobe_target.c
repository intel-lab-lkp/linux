// SPDX-License-Identifier: GPL-2.0
/*
 * tlob_uprobe_target.c - uprobe target binary for tlob selftests.
 *
 * Provides two well-known probe points:
 *   tlob_busy_work()      - start probe: arms the tlob budget timer
 *   tlob_busy_work_done() - stop  probe: cancels the timer on completion
 *
 * The tlob selftest writes a five-field uprobe binding:
 *   pid:threshold_us:binary:offset_start:offset_stop
 * where offset_start is the file offset of tlob_busy_work and offset_stop
 * is the file offset of tlob_busy_work_done (resolved via tlob_helper
 * sym_offset).
 *
 * Both probe points are plain entry uprobes (no uretprobe).  The busy loop
 * keeps the task on-CPU so that either the stop probe fires cleanly (within
 * budget) or the hrtimer fires first and emits tlob_budget_exceeded (over
 * budget).
 *
 * Usage: tlob_uprobe_target <duration_ms>
 *
 * Loops calling tlob_busy_work() in 200 ms iterations until <duration_ms>
 * has elapsed (0 = run for ~24 hours).  Short iterations ensure the uprobe
 * entry fires on every call even if the uprobe is installed after the
 * program has started.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/*
 * tlob_busy_work_done - stop-probe target.
 *
 * Called by tlob_busy_work() after the busy loop.  The uprobe on this
 * function's entry fires tlob_stop_task(), cancelling the budget timer.
 * noinline ensures the compiler never merges this function with its caller,
 * guaranteeing the entry uprobe always fires.
 */
noinline void tlob_busy_work_done(void)
{
	/* empty: the uprobe fires on entry */
}

/*
 * tlob_busy_work - start-probe target.
 *
 * The uprobe on this function's entry fires tlob_start_task(), arming the
 * budget timer.  noinline prevents the compiler and linker (including LTO)
 * from inlining this function into its callers, ensuring the entry uprobe
 * fires on every call.
 */
noinline void tlob_busy_work(unsigned long duration_ns)
{
	struct timespec start, now;
	unsigned long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (unsigned long)(now.tv_sec - start.tv_sec)
			  * 1000000000UL
			+ (unsigned long)(now.tv_nsec - start.tv_nsec);
	} while (elapsed < duration_ns);

	tlob_busy_work_done();
}

int main(int argc, char *argv[])
{
	unsigned long duration_ms = 0;
	struct timespec deadline, now;

	if (argc >= 2)
		duration_ms = strtoul(argv[1], NULL, 10);

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	timespec_add_ms(&deadline, duration_ms ? duration_ms : 86400000UL);

	do {
		tlob_busy_work(200 * 1000000UL); /* 200 ms per iteration */
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (timespec_before(&now, &deadline));

	return 0;
}
