/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test for concurrent parent disable vs child enable race.
 *
 * Tests that when parent scheduler disable happens concurrently with
 * child scheduler enable, the kernel handles the race correctly without
 * UAF (Use-After-Free), memory corruption, or deadlock.
 *
 * Key code path being tested:
 *   scx_sub_enable_workfn() [line 6882+]:
 *     - Sets cgroup->scx_sched (line 6973)
 *     - Checks CSS_ONLINE flag (line 6974)
 *   ↓ RACE ↓
 *   scx_cgroup_lifetime_notify() [responds to CSS going offline]:
 *     - Calls disable_and_exit_task()
 *
 * This can trigger:
 * - The enable workfn starting initialization of tasks
 * - Parent disable path trying to clean up same tasks
 * - Both happening without proper synchronization
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */

#include <bpf/bpf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <scx/common.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "sub_sched_parent.bpf.skel.h"
#include "sub_sched_child.bpf.skel.h"
#include "scx_test.h"

#define TEST_CGROUP_PATH "/sys/fs/cgroup/test_sub_sched_race"

struct test_context {
	struct sub_sched_parent *parent_skel;
	struct sub_sched_child *child_skel;
	char cgroup_path[256];
	pthread_t disable_thread;
	struct bpf_link *parent_link;
	int disable_start_signal;
};

static u64 create_test_cgroup(const char *path)
{
	struct stat st;

	if (mkdir(path, 0755) < 0) {
		if (errno != EEXIST) {
			SCX_ERR("Failed to create cgroup: %s", strerror(errno));
			return -1;
		}
	}

	if (stat(path, &st) < 0) {
		SCX_ERR("Failed to stat cgroup: %s", strerror(errno));
		return -1;
	}

	return st.st_ino;
}

static void cleanup_cgroup(const char *path)
{
	if (rmdir(path) < 0 && errno != ENOENT)
		SCX_ERR("Warning: Failed to cleanup cgroup: %s", strerror(errno));
}

/**
 * Thread function: Disable parent scheduler after a delay
 * This creates the race condition with child enable
 */
static void *disable_thread_fn(void *arg)
{
	struct test_context *ctx = arg;

	/* Wait for signal that child attach is happening */
	while (!__atomic_load_n(&ctx->disable_start_signal, __ATOMIC_ACQUIRE))
		usleep(10000);  /* 10ms */

	/* Small delay to ensure we're mid-initialization */
	usleep(50000);  /* 50ms */

	/* Destroy parent link - this triggers disable path */
	if (ctx->parent_link) {
		bpf_link__destroy(ctx->parent_link);
		ctx->parent_link = NULL;
	}

	return NULL;
}

static enum scx_test_status setup(void **ctx)
{
	struct test_context *test_ctx;
	u64 cgroup_id;

	test_ctx = calloc(1, sizeof(*test_ctx));
	if (!test_ctx)
		return SCX_TEST_FAIL;

	snprintf(test_ctx->cgroup_path, sizeof(test_ctx->cgroup_path),
		 "%s_race.%d", TEST_CGROUP_PATH, getpid());

	cgroup_id = create_test_cgroup(test_ctx->cgroup_path);
	if (cgroup_id == (u64)-1) {
		SCX_ERR("Failed to create test cgroup");
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Load parent scheduler */
	test_ctx->parent_skel = sub_sched_parent__open();
	if (!test_ctx->parent_skel) {
		SCX_ERR("Failed to open parent BPF skeleton");
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	SCX_ENUM_INIT(test_ctx->parent_skel);
	if (sub_sched_parent__load(test_ctx->parent_skel)) {
		SCX_ERR("Failed to load parent BPF program");
		sub_sched_parent__destroy(test_ctx->parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Load child scheduler */
	test_ctx->child_skel = sub_sched_child__open();
	if (!test_ctx->child_skel) {
		SCX_ERR("Failed to open child BPF skeleton");
		sub_sched_parent__destroy(test_ctx->parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	test_ctx->child_skel->struct_ops.sub_sched_child_ops->sub_cgroup_id = cgroup_id;

	SCX_ENUM_INIT(test_ctx->child_skel);
	if (sub_sched_child__load(test_ctx->child_skel)) {
		SCX_ERR("Failed to load child BPF program");
		sub_sched_child__destroy(test_ctx->child_skel);
		sub_sched_parent__destroy(test_ctx->parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	*ctx = test_ctx;
	return SCX_TEST_PASS;
}

/**
 * Run: Test concurrent parent disable vs child enable.
 *
 * This tests the synchronization between:
 * 1. Child scheduler enable workfn (scx_sub_enable_workfn)
 * 2. Parent scheduler disable path (scx_sub_disable)
 *
 * Both can race on:
 * - Task state changes
 * - cgroup->scx_sched pointer updates
 * - CSS_ONLINE checks
 *
 * The kernel must handle this without:
 * - Use-After-Free (UAF)
 * - Memory corruption
 * - Deadlock
 * - Reference count errors
 */
static enum scx_test_status run(void *ctx)
{
	struct test_context *test_ctx = ctx;
	struct bpf_link *child_link;
	int ret;

	/* Attach parent scheduler */
	test_ctx->parent_link = bpf_map__attach_struct_ops(
		test_ctx->parent_skel->maps.sub_sched_parent_ops);
	if (!test_ctx->parent_link) {
		SCX_ERR("Failed to attach parent scheduler");
		return SCX_TEST_FAIL;
	}

	/* Start thread that will disable parent mid-way through child enable */
	test_ctx->disable_start_signal = 0;
	ret = pthread_create(&test_ctx->disable_thread, NULL, disable_thread_fn, test_ctx);
	if (ret) {
		SCX_ERR("Failed to create disable thread: %s", strerror(ret));
		bpf_link__destroy(test_ctx->parent_link);
		return SCX_TEST_FAIL;
	}

	/* Signal the disable thread that we're about to attach child */
	__atomic_store_n(&test_ctx->disable_start_signal, 1, __ATOMIC_RELEASE);

	/*
	 * Try to attach child scheduler.
	 * The disable thread will concurrently try to disable parent.
	 * This should not crash or deadlock.
	 */
	child_link = bpf_map__attach_struct_ops(test_ctx->child_skel->maps.sub_sched_child_ops);

	/* Clean up */
	if (child_link)
		bpf_link__destroy(child_link);

	/* Ensure disable thread finishes */
	pthread_join(test_ctx->disable_thread, NULL);

	/* Verify parent is still cleanly detachable (if not already destroyed) */
	if (test_ctx->parent_link)
		bpf_link__destroy(test_ctx->parent_link);

	/* If we got here without crash, the race was handled correctly */
	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct test_context *test_ctx = ctx;

	if (!test_ctx)
		return;

	if (test_ctx->parent_link)
		bpf_link__destroy(test_ctx->parent_link);

	if (test_ctx->child_skel)
		sub_sched_child__destroy(test_ctx->child_skel);

	if (test_ctx->parent_skel)
		sub_sched_parent__destroy(test_ctx->parent_skel);

	cleanup_cgroup(test_ctx->cgroup_path);
	free(test_ctx);
}

struct scx_test sub_sched_race = {
	.name = "sub_sched_race",
	.description = "Test concurrent parent disable vs child enable race",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};

REGISTER_SCX_TEST(&sub_sched_race)
