// SPDX-License-Identifier: GPL-2.0
/*
 * Test for DSQ operations including create, destroy, and peek operations.
 *
 * Copyright (c) 2025 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2025 Ryan Newton <ryan.newton@alum.mit.edu>
 */
#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sched.h>
#include "peek_dsq.bpf.skel.h"
#include "scx_test.h"

static bool workload_running = true;
static pthread_t workload_thread;

/**
 * Background workload thread that sleeps and wakes rapidly to exercise
 * the scheduler's enqueue operations and ensure DSQ operations get tested.
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
	struct peek_dsq *skel;

	skel = peek_dsq__open();
	SCX_FAIL_IF(!skel, "Failed to open");
	SCX_ENUM_INIT(skel);
	SCX_FAIL_IF(peek_dsq__load(skel), "Failed to load skel");

	*ctx = skel;

	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct peek_dsq *skel = ctx;
	bool failed = false;
	int seconds = 2;
	int err;

	/* Enable the scheduler to test DSQ operations */
	printf("Enabling scheduler to test DSQ insert operations...\n");

	struct bpf_link *link =
		bpf_map__attach_struct_ops(skel->maps.peek_dsq_ops);

	if (!link) {
		SCX_ERR("Failed to attach struct_ops");
		return SCX_TEST_FAIL;
	}

	/* Start background workload thread to exercise the scheduler */
	printf("Starting background workload thread...\n");
	workload_running = true;
	err = pthread_create(&workload_thread, NULL, workload_thread_fn, NULL);
	if (err) {
		SCX_ERR("Failed to create workload thread: %s", strerror(err));
		bpf_link__destroy(link);
		return SCX_TEST_FAIL;
	}

	printf("Waiting for enqueue events.\n");
	sleep(2);
	while (skel->data->enqueue_count <= 0) {
		printf(".");
		fflush(stdout);
		sleep(1);
		seconds++;
		if (seconds >= 30) {
			printf("\n✗ Timeout waiting for enqueue events\n");
			/* Stop workload thread and cleanup */
			workload_running = false;
			pthread_join(workload_thread, NULL);
			bpf_link__destroy(link);
			return SCX_TEST_FAIL;
		}
	}

	workload_running = false;
	err = pthread_join(workload_thread, NULL);
	if (err) {
		SCX_ERR("Failed to join workload thread: %s", strerror(err));
		bpf_link__destroy(link);
		return SCX_TEST_FAIL;
	}
	printf("Background workload thread stopped.\n");

	/* Detach the scheduler */
	bpf_link__destroy(link);

	/* Check if DSQ creation succeeded */
	if (skel->data->dsq_create_result != 1) {
		printf("✗ DSQ create failed: got %d, expected 1\n",
		       skel->data->dsq_create_result);
		failed = true;
	} else {
		printf("✓ DSQ create succeeded\n");
	}

	printf("Enqueue/dispatch count over %d seconds: %d / %d\n", seconds,
		skel->data->enqueue_count, skel->data->dispatch_count);
	printf("Debug: ksym_exists=%d\n",
	       skel->data->debug_ksym_exists);

	/* Check DSQ insert result */
	printf("DSQ insert test done on cpu: %d\n", skel->data->insert_test_cpu);
	if (skel->data->insert_test_cpu != -1)
		printf("✓ DSQ insert succeeded !\n");
	else {
		printf("✗ DSQ insert failed or not attempted\n");
		failed = true;
	}

	/* Check DSQ peek results */
	printf("  DSQ peek result 1 (before insert): %d\n",
	       skel->data->dsq_peek_result1);
	if (skel->data->dsq_peek_result1 == 0)
		printf("✓ DSQ peek verification succeeded - peek returned NULL!\n");
	else {
		printf("✗ DSQ peek verification failed\n");
		failed = true;
	}

	printf("  DSQ peek result 2 (after insert): %ld\n",
	       skel->data->dsq_peek_result2);
	printf("  DSQ peek result 2, expected: %ld\n",
	       skel->data->dsq_peek_result2_expected);
	if (skel->data->dsq_peek_result2 ==
	    skel->data->dsq_peek_result2_expected)
		printf("✓ DSQ peek verification succeeded - peek returned the inserted task!\n");
	else {
		printf("✗ DSQ peek verification failed\n");
		failed = true;
	}

	printf("  Inserted test task -> pid: %ld\n", skel->data->dsq_inserted_pid);
	printf("  DSQ peek result 2 -> pid: %ld\n", skel->data->dsq_peek_result2_pid);

	if (skel->data->dsq_destroy_result != 1) {
		printf("✗ DSQ destroy failed: got %d, expected 1\n",
		       skel->data->dsq_destroy_result);
		failed = true;
	}

	if (failed)
		return SCX_TEST_FAIL;
	else
		return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct peek_dsq *skel = ctx;

	/* Ensure workload thread is stopped */
	if (workload_running) {
		workload_running = false;
		pthread_join(workload_thread, NULL);
	}

	peek_dsq__destroy(skel);
}

struct scx_test peek_dsq = {
	.name = "peek_dsq",
	.description =
		"Test DSQ create/destroy operations and future peek functionality",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&peek_dsq)
