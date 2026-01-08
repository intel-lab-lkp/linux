// SPDX-License-Identifier: GPL-2.0
/*
 * Test for DSQ statistics functionality.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 */
#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sched.h>
#include "dsq_stats.bpf.skel.h"
#include "scx_test.h"

#define NUM_WORKERS 2

static bool workload_running = true;
static pthread_t workload_threads[NUM_WORKERS];

/**
 * Background workload thread that exercises the scheduler to trigger
 * DSQ operations and statistics collection.
 */
static void *workload_thread_fn(void *arg)
{
	while (workload_running) {
		/* Sleep for a very short time to trigger scheduler activity */
		usleep(1000); /* 1ms sleep */
		/* Yield to ensure we go through the scheduler */
		sched_yield();
	}
	return NULL;
}

static enum scx_test_status setup(void **ctx)
{
	struct dsq_stats *skel;
	int i, ret;

	skel = dsq_stats__open();
	SCX_FAIL_IF(!skel, "Failed to open");

	SCX_ENUM_INIT(skel);
	SCX_FAIL_IF(dsq_stats__load(skel), "Failed to load skel");

	/* Start background workload threads */
	for (i = 0; i < NUM_WORKERS; i++) {
		ret = pthread_create(&workload_threads[i], NULL, workload_thread_fn, NULL);
		SCX_FAIL_IF(ret, "Failed to create workload thread %d", i);
	}

	*ctx = skel;

	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct dsq_stats *skel = ctx;
	struct bpf_link *link;
	int duration = 2; /* Run test for 2 seconds */

	link = bpf_map__attach_struct_ops(skel->maps.dsq_stats_ops);
	SCX_FAIL_IF(!link, "Failed to attach scheduler");

	/* Let the scheduler run for a while to collect statistics */
	sleep(duration);

	workload_running = false;

	bpf_link__destroy(link);

	return SCX_TEST_PASS;
}

static enum scx_test_status check_results(void *ctx)
{
	struct dsq_stats *skel = ctx;

	/* Wait for test to complete */
	int timeout = 50; /* 5 seconds timeout */

	while (!skel->bss->test_completed && timeout > 0) {
		usleep(100000); /* 100ms */
		timeout--;
	}

	SCX_FAIL_IF(timeout <= 0, "Test timed out waiting for completion");

	/* Check that statistics were collected */
	SCX_FAIL_IF(skel->bss->test_enqueue_count < 0,
		   "Enqueue count not collected: %lld",
		   (long long)skel->bss->test_enqueue_count);

	SCX_FAIL_IF(skel->bss->test_dequeue_count < 0,
		   "Dequeue count not collected: %lld",
		   (long long)skel->bss->test_dequeue_count);

	SCX_FAIL_IF(skel->bss->test_peak_nr < 0,
		   "Peak NR not collected: %d",
		   skel->bss->test_peak_nr);

	/* Basic sanity checks */
	SCX_FAIL_IF(skel->bss->test_enqueue_count != skel->bss->test_dequeue_count,
		   "Enqueue count (%lld) != Dequeue count (%lld)",
		   (long long)skel->bss->test_enqueue_count,
		   (long long)skel->bss->test_dequeue_count);

	SCX_FAIL_IF(skel->bss->test_peak_nr < skel->bss->current_nr,
		   "Peak NR (%d) < Current NR (%d)",
		   skel->bss->test_peak_nr, skel->bss->current_nr);

	bpf_printk("DSQ Stats Test Results:\n");
	bpf_printk("  Enqueue Count: %lld\n", (long long)skel->bss->test_enqueue_count);
	bpf_printk("  Dequeue Count: %lld\n", (long long)skel->bss->test_dequeue_count);
	bpf_printk("  Peak NR: %d\n", skel->bss->test_peak_nr);
	bpf_printk("  Current NR: %d\n", skel->bss->current_nr);

	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct dsq_stats *skel = ctx;
	int i;

	/* Stop workload threads */
	workload_running = false;
	for (i = 0; i < NUM_WORKERS; i++) {
		if (workload_threads[i])
			pthread_join(workload_threads[i], NULL);
	}

	dsq_stats__destroy(skel);
}

struct scx_test dsq_stats = {
	.name = "dsq_stats",
	.description = "Test DSQ statistics functionality",
	.setup = setup,
	.run = run,
	.check_results = check_results,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&dsq_stats)
