// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 NVIDIA Corporation.
 */
#define _GNU_SOURCE
#include <bpf/bpf.h>
#include <pthread.h>
#include <scx/common.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "numa_nid.bpf.skel.h"
#include "scx_test.h"

#define WORKLOAD_SIZE		(64 << 20)
#define WORKLOAD_THREADS	2
#define WORKLOAD_SECONDS	3

/*
 * Touch a private buffer for a while. The hinting-fault scan skips the pages
 * of a single-threaded process that are already on the node it runs on, so
 * one such thread alone would never take a hinting fault: run two of them.
 */
static void *touch_memory(void *arg)
{
	struct timespec start, now;
	volatile char *buffer;
	size_t offset;

	buffer = malloc(WORKLOAD_SIZE);
	if (!buffer)
		return NULL;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		for (offset = 0; offset < WORKLOAD_SIZE; offset += 4096)
			buffer[offset]++;
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (now.tv_sec - start.tv_sec < WORKLOAD_SECONDS);

	free((void *)buffer);
	return NULL;
}

static enum scx_test_status setup(void **ctx)
{
	struct numa_nid *skel;

	skel = numa_nid__open();
	SCX_FAIL_IF(!skel, "Failed to open");
	SCX_ENUM_INIT(skel);
	SCX_FAIL_IF(numa_nid__load(skel), "Failed to load skel");

	*ctx = skel;

	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct numa_nid *skel = ctx;
	pthread_t threads[WORKLOAD_THREADS - 1];
	struct bpf_link *link;
	int i;

	link = bpf_map__attach_struct_ops(skel->maps.numa_nid_ops);
	SCX_FAIL_IF(!link, "Failed to attach scheduler");

	/*
	 * Give the scan something to work on, so that the tasks of this test
	 * can acquire a preferred node on a NUMA machine with numa_balancing
	 * enabled. The test does not require one, as it depends on the
	 * machine: the scheduler reports an error through UEI if it ever sees
	 * a node id that is not valid and the counts below show what it saw.
	 */
	for (i = 0; i < WORKLOAD_THREADS - 1; i++) {
		if (pthread_create(&threads[i], NULL, touch_memory, NULL)) {
			SCX_ERR("Failed to create a worker thread");
			bpf_link__destroy(link);
			return SCX_TEST_FAIL;
		}
	}
	touch_memory(NULL);
	for (i = 0; i < WORKLOAD_THREADS - 1; i++)
		pthread_join(threads[i], NULL);

	fprintf(stderr, "tasks with a preferred node: %lu, without: %lu\n",
		(unsigned long)skel->bss->nr_with_nid,
		(unsigned long)skel->bss->nr_without_nid);

	bpf_link__destroy(link);
	SCX_EQ(skel->data->uei.kind, EXIT_KIND(SCX_EXIT_UNREG));

	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct numa_nid *skel = ctx;

	numa_nid__destroy(skel);
}

struct scx_test numa_nid = {
	.name = "numa_nid",
	.description = "Read a task's preferred NUMA node from BPF",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&numa_nid)
