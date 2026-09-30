// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Changbin Du
 *
 * Benchmark for atomic compare-and-swap (CAS) operations.
 *
 * Measures throughput (ops/sec) of contended CAS on a single shared
 * counter across multiple threads. Each thread performs a hot loop
 * of compare-and-swap, competing with all other threads for the
 * same cache line.
 */
#include "bench.h"
#include <linux/compiler.h>
#include "../util/debug.h"

#ifndef HAVE_PTHREAD_BARRIER
int bench_atomic(int argc __maybe_unused, const char **argv __maybe_unused)
{
	pr_err("%s: pthread_barrier_t unavailable, disabling this test...\n", __func__);
	return 0;
}
#else /* HAVE_PTHREAD_BARRIER */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <inttypes.h>
#include <err.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/time64.h>
#include <subcmd/parse-options.h>
#include "../util/stat.h"

static unsigned int threads	= 2;
static unsigned int iterations	= 100000000;

static const struct option options[] = {
	OPT_UINTEGER('t', "threads", &threads,
		     "Number of threads contending for the counter"),
	OPT_UINTEGER('i', "iterations", &iterations,
		     "Number of iterations per thread"),
	OPT_END()
};

static const char * const bench_usage[] = {
	"perf bench atomic cas <options>",
	NULL
};

static u64 shared_counter __aligned(64);
static pthread_barrier_t start_barrier __aligned(64);

struct worker_stats {
	u64	runtime_ns;
};

static void *worker(void *arg)
{
	struct worker_stats *ws = (struct worker_stats *)arg;
	struct timespec tstart, tend;
	u64 i;

	pthread_barrier_wait(&start_barrier);

	clock_gettime(CLOCK_MONOTONIC, &tstart);
	for (i = 0; i < iterations; i++) {
		u64 old_val, new_val;

		do {
			old_val = __atomic_load_n(&shared_counter,
						  __ATOMIC_RELAXED);
			new_val = old_val + 1;
		} while (!__atomic_compare_exchange_n(&shared_counter,
						      &old_val, new_val,
						      false,
						      __ATOMIC_RELAXED,
						      __ATOMIC_RELAXED));
	}
	clock_gettime(CLOCK_MONOTONIC, &tend);

	ws->runtime_ns	= (tend.tv_sec - tstart.tv_sec) * NSEC_PER_SEC +
			  (tend.tv_nsec - tstart.tv_nsec);

	return NULL;
}

static void print_default_format(struct worker_stats *wstats,
				 struct stats *time_stats)
{
	double time_avg, time_stddev;

	printf("  Threads: %u, iterations/thread: %u, repeats: %u (warmup: 1)\n",
	       threads, iterations, bench_repeat);

	time_avg    = avg_stats(time_stats);
	time_stddev = stddev_stats(time_stats);

	printf("  Avg wall-clock time: %.3f msec  (stddev %.3f msec)\n",
	       time_avg / (double)NSEC_PER_MSEC,
	       time_stddev / (double)NSEC_PER_MSEC);
	printf("  Total ops: %'" PRIu64 "\n",
	       iterations * (u64)threads);
	printf("  Throughput total: %'.0f ops/sec\n",
	       (double)(iterations * (u64)threads) /
	       (time_avg / (double)NSEC_PER_SEC));

	/*
	 * Per-thread statistics from the last measured repeat show
	 * fairness / imbalance in CPU scheduling.
	 */
	if (threads > 1) {
		u64 min_ns = UINT64_MAX, max_ns = 0, total_ns = 0;
		double min_thru, max_thru, avg_thru;

		for (unsigned int t = 0; t < threads; t++) {
			if (wstats[t].runtime_ns < min_ns)
				min_ns = wstats[t].runtime_ns;
			if (wstats[t].runtime_ns > max_ns)
				max_ns = wstats[t].runtime_ns;
			total_ns += wstats[t].runtime_ns;
		}

		min_thru = (double)iterations * NSEC_PER_SEC / min_ns;
		max_thru = (double)iterations * NSEC_PER_SEC / max_ns;
		avg_thru = (double)threads * iterations * NSEC_PER_SEC / total_ns;

		printf("  Per-thread times and throughput (last repeat):\n");
		printf("    fastest:  %.3f msec  (%.0f ops/sec)\n",
		       min_ns / (double)NSEC_PER_MSEC, min_thru);
		printf("    slowest:  %.3f msec  (%.0f ops/sec)\n",
		       max_ns / (double)NSEC_PER_MSEC, max_thru);
		printf("    avg:      %.3f msec  (%.0f ops/sec)\n",
		       (total_ns / threads) / (double)NSEC_PER_MSEC, avg_thru);
	}
}

static int run_cas_benchmark(void)
{
	pthread_t *thread_ids;
	struct worker_stats *wstats;
	struct stats time_stats;
	unsigned int r, t;

	thread_ids = calloc(threads, sizeof(pthread_t));
	if (!thread_ids)
		return -1;

	wstats = calloc(threads, sizeof(struct worker_stats));
	if (!wstats) {
		free(thread_ids);
		return -1;
	}

	init_stats(&time_stats);

	for (r = 0; r < bench_repeat + 1; r++) {
		u64 max_runtime_ns = 0;

		shared_counter = 0;
		pthread_barrier_init(&start_barrier, NULL, threads);

		for (t = 0; t < threads; t++) {
			if (pthread_create(&thread_ids[t], NULL,
					   worker, &wstats[t]))
				err(EXIT_FAILURE, "pthread_create");
		}

		for (t = 0; t < threads; t++)
			pthread_join(thread_ids[t], NULL);

		pthread_barrier_destroy(&start_barrier);

		for (t = 0; t < threads; t++) {
			if (wstats[t].runtime_ns > max_runtime_ns)
				max_runtime_ns = wstats[t].runtime_ns;
		}

		/*
		 * Exclude the first repeat (warmup) from statistics so
		 * that cache-cold and lazy-init effects are not counted.
		 */
		if (r > 0)
			update_stats(&time_stats, max_runtime_ns);
	}

	switch (bench_format) {
	case BENCH_FORMAT_DEFAULT:
		print_default_format(wstats, &time_stats);
		break;

	case BENCH_FORMAT_SIMPLE:
		printf("%.0f\n",
		       (double)(iterations * (u64)threads) /
		       (avg_stats(&time_stats) / (double)NSEC_PER_SEC));
		break;

	default:
		fprintf(stderr, "Unknown format: %d\n", bench_format);
		exit(EXIT_FAILURE);
	}

	free(thread_ids);
	free(wstats);

	return 0;
}

int bench_atomic(int argc, const char **argv)
{
	if (parse_options(argc, argv, options, bench_usage, 0)) {
		usage_with_options(bench_usage, options);
		exit(EXIT_FAILURE);
	}

	if (threads < 1) {
		fprintf(stderr, "Invalid thread count: %u\n", threads);
		return 1;
	}

	if (iterations < 1) {
		fprintf(stderr, "Invalid iteration count: %u\n", iterations);
		return 1;
	}

	return run_cas_benchmark();
}
#endif /* HAVE_PTHREAD_BARRIER */
