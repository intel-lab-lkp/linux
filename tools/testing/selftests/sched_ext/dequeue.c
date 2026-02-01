// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 NVIDIA Corporation.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include <sched.h>
#include <pthread.h>
#include "scx_test.h"
#include "dequeue.bpf.skel.h"

#define NUM_WORKERS 8

/*
 * Worker function that creates enqueue/dequeue events. It alternates
 * between CPU work, sleeping, and affinity changes to trigger dequeues.
 */
static void worker_fn(int id)
{
	cpu_set_t cpuset;
	int i;
	volatile int sum = 0;

	for (i = 0; i < 1000; i++) {
		int j;

		/* Do some work to trigger scheduling events */
		for (j = 0; j < 10000; j++)
			sum += j;

		/* Change affinity to trigger dequeue */
		if (i % 10 == 0) {
			CPU_ZERO(&cpuset);
			/* Rotate through the first 4 CPUs */
			CPU_SET(i % 4, &cpuset);
			sched_setaffinity(0, sizeof(cpuset), &cpuset);
		}

		/* Do additional work */
		for (j = 0; j < 10000; j++)
			sum += j;

		/* Sleep to trigger dequeue */
		usleep(1000 + (id * 100));
	}

	exit(0);
}

static enum scx_test_status run_scenario(struct dequeue *skel, u32 scenario,
					 const char *scenario_name)
{
	struct bpf_link *link;
	pid_t pids[NUM_WORKERS];
	int i, status;
	u64 enq_start, deq_start, dispatch_deq_start, change_deq_start;
	u64 enq_delta, deq_delta, dispatch_deq_delta, change_deq_delta;

	/* Set the test scenario */
	skel->bss->test_scenario = scenario;

	/* Record starting counts */
	enq_start = skel->bss->enqueue_cnt;
	deq_start = skel->bss->dequeue_cnt;
	dispatch_deq_start = skel->bss->dispatch_dequeue_cnt;
	change_deq_start = skel->bss->change_dequeue_cnt;

	link = bpf_map__attach_struct_ops(skel->maps.dequeue_ops);
	SCX_FAIL_IF(!link, "Failed to attach struct_ops for scenario %s", scenario_name);

	/* Fork worker processes to generate enqueue/dequeue events */
	for (i = 0; i < NUM_WORKERS; i++) {
		pids[i] = fork();
		SCX_FAIL_IF(pids[i] < 0, "Failed to fork worker %d", i);

		if (pids[i] == 0) {
			worker_fn(i);
			/* Should not reach here */
			exit(1);
		}
	}

	/* Wait for all workers to complete */
	for (i = 0; i < NUM_WORKERS; i++) {
		SCX_FAIL_IF(waitpid(pids[i], &status, 0) != pids[i],
			    "Failed to wait for worker %d", i);
		SCX_FAIL_IF(status != 0, "Worker %d exited with status %d", i, status);
	}

	bpf_link__destroy(link);

	SCX_EQ(skel->data->uei.kind, EXIT_KIND(SCX_EXIT_UNREG));

	/* Calculate deltas */
	enq_delta = skel->bss->enqueue_cnt - enq_start;
	deq_delta = skel->bss->dequeue_cnt - deq_start;
	dispatch_deq_delta = skel->bss->dispatch_dequeue_cnt - dispatch_deq_start;
	change_deq_delta = skel->bss->change_dequeue_cnt - change_deq_start;

	printf("%s:\n", scenario_name);
	printf("  enqueues: %lu\n", (unsigned long)enq_delta);
	printf("  dequeues: %lu (dispatch: %lu, property_change: %lu)\n",
	       (unsigned long)deq_delta,
	       (unsigned long)dispatch_deq_delta,
	       (unsigned long)change_deq_delta);

	/*
	 * Validate enqueue/dequeue lifecycle tracking.
	 *
	 * For scenario 0 (Local DSQ), both enqueues and dequeues should be
	 * 0 because tasks bypass the BPF scheduler entirely: they never
	 * enter BPF scheduler's custody. For scenario 1 (ser DSQ) , we
	 * expect both enqueues and dequeues.
	 *
	 * The BPF code does strict state machine validation with
	 * scx_bpf_error() to ensure the workflow semantics are correct. If
	 * we reach here without errors, the semantics are validated
	 * correctly.
	 */
	if (scenario == 0) {
		/* Local DSQ: tasks bypass BPF scheduler completely */
		SCX_EQ(enq_delta, 0);
		SCX_EQ(deq_delta, 0);
		SCX_EQ(dispatch_deq_delta, 0);
		SCX_EQ(change_deq_delta, 0);
	} else {
		/* Non-local DSQ: tasks enter BPF scheduler's custody */
		SCX_GT(enq_delta, 0);
		SCX_GT(deq_delta, 0);
		/* Validate 1:1 enqueue/dequeue pairing */
		SCX_EQ(enq_delta, deq_delta);
	}

	return SCX_TEST_PASS;
}

static enum scx_test_status setup(void **ctx)
{
	struct dequeue *skel;

	skel = dequeue__open();
	SCX_FAIL_IF(!skel, "Failed to open skel");
	SCX_ENUM_INIT(skel);
	SCX_FAIL_IF(dequeue__load(skel), "Failed to load skel");

	*ctx = skel;

	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct dequeue *skel = ctx;
	enum scx_test_status status;

	status = run_scenario(skel, 0, "Local DSQ (direct dispatch)");
	if (status != SCX_TEST_PASS)
		return status;

	status = run_scenario(skel, 1, "User DSQ");
	if (status != SCX_TEST_PASS)
		return status;

	printf("\n=== Summary ===\n");
	printf("Total enqueues: %lu\n", (unsigned long)skel->bss->enqueue_cnt);
	printf("Total dequeues: %lu\n", (unsigned long)skel->bss->dequeue_cnt);
	printf("  Dispatch dequeues: %lu (no flag, normal workflow)\n",
	       (unsigned long)skel->bss->dispatch_dequeue_cnt);
	printf("  Property change dequeues: %lu (SCX_DEQ_SCHED_CHANGE flag)\n",
	       (unsigned long)skel->bss->change_dequeue_cnt);
	printf("\nAll scenarios passed - no state machine violations detected\n");
	printf("-> Validated: Local DSQ dispatch bypasses BPF scheduler (no dequeue callbacks)\n");
	printf("-> Validated: Non-local DSQ dispatch triggers dequeue callbacks\n");
	printf("-> Validated: Dispatch dequeues have no flags (normal workflow)\n");
	printf("-> Validated: Async dequeues have SCX_DEQ_SCHED_CHANGE flag (interruptions)\n");
	printf("-> Validated: No duplicate enqueues or invalid state transitions\n");

	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct dequeue *skel = ctx;

	dequeue__destroy(skel);
}

struct scx_test dequeue_test = {
	.name = "dequeue",
	.description = "Verify ops.dequeue() semantics for local and non-local DSQ dispatch",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};

REGISTER_SCX_TEST(&dequeue_test)
