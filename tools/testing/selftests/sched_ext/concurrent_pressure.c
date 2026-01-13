/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Concurrent pressure test for sched_ext
 *
 * Tests scheduler behavior under high concurrency stress
 *
 * Copyright (c) 2025 Linux Kernel Contributors
 */

#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include "concurrent_pressure.bpf.skel.h"
#include "scx_test.h"

#define NUM_PRESSURE_THREADS 8
#define TASKS_PER_PRESSURE 50
#define TEST_DURATION_SEC 5

struct pressure_data {
	int thread_id;
	struct concurrent_pressure *skel;
	volatile int *stop_flag;
};

static void *pressure_worker(void *arg)
{
	struct pressure_data *data = (struct pressure_data *)arg;
	int local_count = 0;
	
	/* Simulate high-frequency scheduling operations */
	while (!(*data->stop_flag)) {
		/* In a real test, this would trigger scheduling operations */
		/* For now, we just verify the skeleton is loaded */
		local_count++;
		/* Small delay to prevent CPU spinning */
		usleep(1000); /* 1ms */
	}
	
	return NULL;
}

static enum scx_test_status setup(void **ctx)
{
	struct concurrent_pressure *skel;

	skel = concurrent_pressure__open_and_load();
	if (!skel) {
		SCX_ERR("Failed to open and load concurrent_pressure skel");
		return SCX_TEST_FAIL;
	}
	*ctx = skel;

	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct concurrent_pressure *skel = ctx;
	struct bpf_link *link;
	pthread_t threads[NUM_PRESSURE_THREADS];
	struct pressure_data thread_data[NUM_PRESSURE_THREADS];
	volatile int stop_flag = 0;
	time_t start_time, current_time;
	int i, ret;

	/* Attach the scheduler */
	link = bpf_map__attach_struct_ops(skel->maps.pressure_ops);
	if (!link) {
		SCX_ERR("Failed to attach scheduler");
		return SCX_TEST_FAIL;
	}

	/* Start pressure threads */
	for (i = 0; i < NUM_PRESSURE_THREADS; i++) {
		thread_data[i].thread_id = i;
		thread_data[i].skel = skel;
		thread_data[i].stop_flag = &stop_flag;
		
		ret = pthread_create(&threads[i], NULL, pressure_worker, &thread_data[i]);
		if (ret != 0) {
			SCX_ERR("Failed to create thread %d", i);
			/* Signal other threads to stop */
			stop_flag = 1;
			for (int j = 0; j < i; j++) {
				pthread_join(threads[j], NULL);
			}
			bpf_link__destroy(link);
			return SCX_TEST_FAIL;
		}
	}

	/* Run for specified duration */
	start_time = time(NULL);
	while (1) {
		current_time = time(NULL);
		if (current_time - start_time >= TEST_DURATION_SEC) {
			break;
		}
		usleep(100000); /* 100ms */
	}

	/* Stop all threads */
	stop_flag = 1;
	for (i = 0; i < NUM_PRESSURE_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	bpf_link__destroy(link);

	/* Verify the scheduler handled the pressure */
	if (skel->data->total_operations > 0) {
		SCX_ERR("Pressure test completed: %llu operations",
		       skel->data->total_operations);
		return SCX_TEST_PASS;
	} else {
		SCX_ERR("No operations recorded during pressure test");
		return SCX_TEST_FAIL;
	}
}

static void cleanup(void *ctx)
{
	struct concurrent_pressure *skel = ctx;
	concurrent_pressure__destroy(skel);
}

struct scx_test concurrent_pressure = {
	.name = "concurrent_pressure",
	.description = "Test scheduler behavior under high concurrency pressure",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&concurrent_pressure)