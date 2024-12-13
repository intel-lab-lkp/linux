// SPDX-License-Identifier: LGPL-2.1
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "../kselftest.h"
#include "rseq.h"

#define VERBOSE 0
#define printf_verbose(fmt, ...)                    \
	do {                                        \
		if (VERBOSE)                        \
			printf(fmt, ##__VA_ARGS__); \
	} while (0)

/* 0.5 s */
#define RUNNER_PERIOD 500000
/* Number of runs before we terminate or get the token */
#define THREAD_RUNS 5

/*
 * Number of times we check that the mm_cid were compacted.
 * Checks are repeated every RUNNER_PERIOD
 */
#define MM_CID_CLEANUP_TIMEOUT 10

struct thread_args {
	int num_cpus;
	pthread_mutex_t token;
	pthread_t *tinfo;
};

static void *thread_runner(void *arg)
{
	struct thread_args *args = arg;
	int i, ret, curr_mm_cid;

	for (i = 0; i < THREAD_RUNS; i++)
		usleep(RUNNER_PERIOD);
	curr_mm_cid = rseq_current_mm_cid();
	/*
	 * We select one thread with high enough mm_cid to be the new leader
	 * all other threads (including the main thread) will terminate
	 * After some time, the mm_cid of the only remaining thread should
	 * converge to 0, if not, the test fails
	 */
	if (curr_mm_cid > args->num_cpus / 2 &&
	    !pthread_mutex_trylock(&args->token)) {
		printf_verbose("cpu%d has %d and will be the new leader\n",
			       sched_getcpu(), curr_mm_cid);
		for (i = 0; i < args->num_cpus; i++) {
			if (args->tinfo[i] == pthread_self())
				continue;
			ret = pthread_join(args->tinfo[i], NULL);
			if (ret) {
				fprintf(stderr,
					"Error: failed to join thread %d (%d): %s\n",
					i, ret, strerror(ret));
				assert(ret == 0);
			}
		}
		free(args->tinfo);

		for (i = 0; i < MM_CID_CLEANUP_TIMEOUT; i++) {
			curr_mm_cid = rseq_current_mm_cid();
			printf_verbose("run %d: mm_cid %d on cpu%d\n", i,
				       curr_mm_cid, sched_getcpu());
			if (curr_mm_cid == 0) {
				printf_verbose(
					"mm_cids successfully compacted, exiting\n");
				pthread_exit(NULL);
			}
			usleep(RUNNER_PERIOD);
		}
		assert(false);
	}
	printf_verbose("cpu%d has %d and is going to terminate\n",
		       sched_getcpu(), curr_mm_cid);
	pthread_exit(NULL);
}

void test_mm_cid_compaction(void)
{
	cpu_set_t affinity, test_affinity;
	int i, j, ret, num_threads;
	pthread_t *tinfo;
	struct thread_args args = { .token = PTHREAD_MUTEX_INITIALIZER };

	sched_getaffinity(0, sizeof(affinity), &affinity);
	CPU_ZERO(&test_affinity);
	num_threads = CPU_COUNT(&affinity);
	tinfo = calloc(num_threads, sizeof(*tinfo));
	if (!tinfo) {
		fprintf(stderr, "Error: failed to allocate tinfo(%d): %s\n",
			errno, strerror(errno));
		assert(ret == 0);
	}
	args.num_cpus = num_threads;
	args.tinfo = tinfo;
	if (num_threads == 1) {
		printf_verbose(
			"Running on a single cpu, cannot test anything\n");
		return;
	}
	for (i = 0, j = 0; i < CPU_SETSIZE && j < num_threads; i++) {
		if (CPU_ISSET(i, &affinity)) {
			ret = pthread_create(&tinfo[j], NULL, thread_runner,
					     &args);
			if (ret) {
				fprintf(stderr,
					"Error: failed to create thread(%d): %s\n",
					ret, strerror(ret));
				assert(ret == 0);
			}
			CPU_SET(i, &test_affinity);
			pthread_setaffinity_np(tinfo[j], sizeof(test_affinity),
					       &test_affinity);
			CPU_CLR(i, &test_affinity);
			++j;
		}
	}
	printf_verbose("Started %d threads\n", num_threads);

	/* Also main thread will terminate if it is not selected as leader */
	thread_runner(&args);
}

int main(int argc, char **argv)
{
	if (rseq_register_current_thread()) {
		fprintf(stderr,
			"Error: rseq_register_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto error;
	}
	if (!rseq_mm_cid_available()) {
		fprintf(stderr, "Error: rseq_mm_cid unavailable\n");
		goto error;
	}
	test_mm_cid_compaction();
	if (rseq_unregister_current_thread()) {
		fprintf(stderr,
			"Error: rseq_unregister_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto error;
	}
	return 0;

error:
	return -1;
}
