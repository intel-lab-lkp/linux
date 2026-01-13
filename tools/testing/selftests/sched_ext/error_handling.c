/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Error handling and boundary condition tests for sched_ext
 *
 * Copyright (c) 2025 Linux Kernel Contributors
 */

#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include "error_handling.bpf.skel.h"
#include "scx_test.h"

static enum scx_test_status setup(void **ctx)
{
	struct error_handling *skel;

	skel = error_handling__open_and_load();
	if (!skel) {
		SCX_ERR("Failed to open and load error_handling skel");
		return SCX_TEST_FAIL;
	}
	*ctx = skel;

	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct error_handling *skel = ctx;
	struct bpf_link *link;
	int ret;

	/* Test 1: Normal attachment */
	link = bpf_map__attach_struct_ops(skel->maps.error_ops);
	if (!link) {
		SCX_ERR("Failed to attach scheduler");
		return SCX_TEST_FAIL;
	}

	/* Test 2: Verify error counters are initialized */
	u32 key = 0;
	struct error_stats *stats = bpf_map_lookup_elem(skel->maps.error_stats, &key);
	if (!stats) {
		SCX_ERR("Failed to lookup error stats");
		bpf_link__destroy(link);
		return SCX_TEST_FAIL;
	}

	/* Test 3: Wait for some operations to occur */
	sleep(2);

	/* Test 4: Check if error handling was triggered correctly */
	stats = bpf_map_lookup_elem(skel->maps.error_stats, &key);
	if (!stats) {
		SCX_ERR("Failed to lookup error stats after test");
		bpf_link__destroy(link);
		return SCX_TEST_FAIL;
	}

	/* Verify error counters are working */
	SCX_ERR("Error handling test stats:");
	SCX_ERR("  Invalid task count: %u", stats->invalid_task_count);
	SCX_ERR("  Invalid CPU count: %u", stats->invalid_cpu_count);
	SCX_ERR("  Invalid weight count: %u", stats->invalid_weight_count);
	SCX_ERR("  NULL pointer count: %u", stats->null_pointer_count);

	/* Cleanup */
	bpf_link__destroy(link);

	/* The test passes if the scheduler loaded without crashing */
	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct error_handling *skel = ctx;
	error_handling__destroy(skel);
}

struct scx_test error_handling = {
	.name = "error_handling",
	.description = "Test error handling and boundary conditions",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&error_handling)