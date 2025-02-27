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
 * Checks are repeated every RUNNER_PERIOD.
 */
#define MM_CID_COMPACT_TIMEOUT 10

struct thread_args {
	int cpu;
	int num_cpus;
	pthread_mutex_t *token;
	pthread_barrier_t *barrier;
	pthread_t *tinfo;
	struct thread_args *args_head;
};

static void __noreturn *thread_runner(void *arg)
{
	struct thread_args *args = arg;
	int i, ret, curr_mm_cid;
	cpu_set_t cpumask;

	CPU_ZERO(&cpumask);
	CPU_SET(args->cpu, &cpumask);
	ret = pthread_setaffinity_np(pthread_self(), sizeof(cpumask), &cpumask);
	if (ret) {
		errno = ret;
		perror("Error: failed to set affinity");
		abort();
	}
	pthread_barrier_wait(args->barrier);

	for (i = 0; i < THREAD_RUNS; i++)
		usleep(RUNNER_PERIOD);
	curr_mm_cid = rseq_current_mm_cid();
	/*
	 * We select one thread with high enough mm_cid to be the new leader.
	 * All other threads (including the main thread) will terminate.
	 * After some time, the mm_cid of the only remaining thread should
	 * converge to 0, if not, the test fails.
	 */
	if (curr_mm_cid >= args->num_cpus / 2 &&
	    !pthread_mutex_trylock(args->token)) {
		printf_verbose(
			"cpu%d has mm_cid=%d and will be the new leader.\n",
			sched_getcpu(), curr_mm_cid);
		for (i = 0; i < args->num_cpus; i++) {
			if (args->tinfo[i] == pthread_self())
				continue;
			ret = pthread_join(args->tinfo[i], NULL);
			if (ret) {
				errno = ret;
				perror("Error: failed to join thread");
				abort();
			}
		}
		pthread_barrier_destroy(args->barrier);
		free(args->tinfo);
		free(args->token);
		free(args->barrier);
		free(args->args_head);

		for (i = 0; i < MM_CID_COMPACT_TIMEOUT; i++) {
			curr_mm_cid = rseq_current_mm_cid();
			printf_verbose("run %d: mm_cid=%d on cpu%d.\n", i,
				       curr_mm_cid, sched_getcpu());
			if (curr_mm_cid == 0)
				exit(EXIT_SUCCESS);
			usleep(RUNNER_PERIOD);
		}
		exit(EXIT_FAILURE);
	}
	printf_verbose("cpu%d has mm_cid=%d and is going to terminate.\n",
		       sched_getcpu(), curr_mm_cid);
	pthread_exit(NULL);
}

int test_mm_cid_compaction(void)
{
	cpu_set_t affinity;
	int i, j, ret = 0, num_threads;
	pthread_t *tinfo;
	pthread_mutex_t *token;
	pthread_barrier_t *barrier;
	struct thread_args *args;

	sched_getaffinity(0, sizeof(affinity), &affinity);
	num_threads = CPU_COUNT(&affinity);
	tinfo = calloc(num_threads, sizeof(*tinfo));
	if (!tinfo) {
		perror("Error: failed to allocate tinfo");
		return -1;
	}
	args = calloc(num_threads, sizeof(*args));
	if (!args) {
		perror("Error: failed to allocate args");
		ret = -1;
		goto out_free_tinfo;
	}
	token = malloc(sizeof(*token));
	if (!token) {
		perror("Error: failed to allocate token");
		ret = -1;
		goto out_free_args;
	}
	barrier = malloc(sizeof(*barrier));
	if (!barrier) {
		perror("Error: failed to allocate barrier");
		ret = -1;
		goto out_free_token;
	}
	if (num_threads == 1) {
		fprintf(stderr, "Cannot test on a single cpu. "
				"Skipping mm_cid_compaction test.\n");
		/* only skipping the test, this is not a failure */
		goto out_free_barrier;
	}
	pthread_mutex_init(token, NULL);
	ret = pthread_barrier_init(barrier, NULL, num_threads);
	if (ret) {
		errno = ret;
		perror("Error: failed to initialise barrier");
		goto out_free_barrier;
	}
	for (i = 0, j = 0; i < CPU_SETSIZE && j < num_threads; i++) {
		if (!CPU_ISSET(i, &affinity))
			continue;
		args[j].num_cpus = num_threads;
		args[j].tinfo = tinfo;
		args[j].token = token;
		args[j].barrier = barrier;
		args[j].cpu = i;
		args[j].args_head = args;
		if (!j) {
			/* The first thread is the main one */
			tinfo[0] = pthread_self();
			++j;
			continue;
		}
		ret = pthread_create(&tinfo[j], NULL, thread_runner, &args[j]);
		if (ret) {
			errno = ret;
			perror("Error: failed to create thread");
			abort();
		}
		++j;
	}
	printf_verbose("Started %d threads.\n", num_threads);

	/* Also main thread will terminate if it is not selected as leader */
	thread_runner(&args[0]);

	/* only reached in case of errors */
out_free_barrier:
	free(barrier);
out_free_token:
	free(token);
out_free_args:
	free(args);
out_free_tinfo:
	free(tinfo);

	return ret;
}

int main(int argc, char **argv)
{
	if (!rseq_mm_cid_available()) {
		fprintf(stderr, "Error: rseq_mm_cid unavailable\n");
		return -1;
	}
	if (test_mm_cid_compaction())
		return -1;
	return 0;
}
