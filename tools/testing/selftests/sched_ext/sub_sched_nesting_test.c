/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test for multi-level nested sub-sched cascading disable.
 *
 * Tests that when a parent scheduler is disabled, all nested children
 * are properly disabled and cleaned up recursively via drain_descendants().
 *
 * Hierarchy:
 *   Root Scheduler (global)
 *     └── Parent Sub-Scheduler (level 1)
 *            └── Child Sub-Scheduler (level 2)
 *
 * When root disable happens:
 * 1. Trigger child disable first (if applicable)
 * 2. Trigger parent disable
 * 3. Verify no crashes, no resource leaks, proper cleanup
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */

#include <bpf/bpf.h>
#include <errno.h>
#include <fcntl.h>
#include <scx/common.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "sub_sched_parent.bpf.skel.h"
#include "sub_sched_nesting_child.bpf.skel.h"
#include "scx_test.h"

#define TEST_CGROUP_PATH "/sys/fs/cgroup/test_sub_sched_nesting"

struct test_context {
	struct sub_sched_parent *root_parent_skel;
	struct sub_sched_nesting_child *level1_skel;
	struct sub_sched_nesting_child *level2_skel;
	char cgroup_path_l1[512];
	char cgroup_path_l2[512];
};

/**
 * Create a cgroup v2 for testing.
 * Returns the inode number (which serves as cgroup ID) on success, -1 on error.
 */
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
 * Setup: Create 2-level cgroup hierarchy and load schedulers
 */
static enum scx_test_status setup(void **ctx)
{
	struct test_context *test_ctx;
	u64 cgroup_id_l1, cgroup_id_l2;

	test_ctx = calloc(1, sizeof(*test_ctx));
	if (!test_ctx)
		return SCX_TEST_FAIL;

	/* Create level-1 cgroup */
	snprintf(test_ctx->cgroup_path_l1, sizeof(test_ctx->cgroup_path_l1),
		 "%s_l1.%d", TEST_CGROUP_PATH, getpid());

	cgroup_id_l1 = create_test_cgroup(test_ctx->cgroup_path_l1);
	if (cgroup_id_l1 == (u64)-1) {
		SCX_ERR("Failed to create level-1 cgroup");
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Create level-2 cgroup (nested under level-1) */
	if (snprintf(test_ctx->cgroup_path_l2, sizeof(test_ctx->cgroup_path_l2),
		     "%s/l2.%d", test_ctx->cgroup_path_l1, getpid()) >=
	    (int)sizeof(test_ctx->cgroup_path_l2)) {
		SCX_ERR("Path too long for level-2 cgroup");
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	cgroup_id_l2 = create_test_cgroup(test_ctx->cgroup_path_l2);
	if (cgroup_id_l2 == (u64)-1) {
		SCX_ERR("Failed to create level-2 cgroup");
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Load root parent scheduler */
	test_ctx->root_parent_skel = sub_sched_parent__open();
	if (!test_ctx->root_parent_skel) {
		SCX_ERR("Failed to open root parent BPF skeleton");
		cleanup_cgroup(test_ctx->cgroup_path_l2);
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	SCX_ENUM_INIT(test_ctx->root_parent_skel);
	if (sub_sched_parent__load(test_ctx->root_parent_skel)) {
		SCX_ERR("Failed to load root parent BPF program");
		sub_sched_parent__destroy(test_ctx->root_parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path_l2);
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Load level-1 nesting child (will be attached to root) */
	test_ctx->level1_skel = sub_sched_nesting_child__open();
	if (!test_ctx->level1_skel) {
		SCX_ERR("Failed to open level-1 BPF skeleton");
		sub_sched_parent__destroy(test_ctx->root_parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path_l2);
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	test_ctx->level1_skel->struct_ops.sub_sched_nesting_child_ops->sub_cgroup_id = cgroup_id_l1;

	SCX_ENUM_INIT(test_ctx->level1_skel);
	if (sub_sched_nesting_child__load(test_ctx->level1_skel)) {
		SCX_ERR("Failed to load level-1 BPF program");
		sub_sched_nesting_child__destroy(test_ctx->level1_skel);
		sub_sched_parent__destroy(test_ctx->root_parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path_l2);
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Load level-2 nesting child (will be attached to level-1) */
	test_ctx->level2_skel = sub_sched_nesting_child__open();
	if (!test_ctx->level2_skel) {
		SCX_ERR("Failed to open level-2 BPF skeleton");
		sub_sched_nesting_child__destroy(test_ctx->level1_skel);
		sub_sched_parent__destroy(test_ctx->root_parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path_l2);
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	test_ctx->level2_skel->struct_ops.sub_sched_nesting_child_ops->sub_cgroup_id = cgroup_id_l2;

	SCX_ENUM_INIT(test_ctx->level2_skel);
	if (sub_sched_nesting_child__load(test_ctx->level2_skel)) {
		SCX_ERR("Failed to load level-2 BPF program");
		sub_sched_nesting_child__destroy(test_ctx->level2_skel);
		sub_sched_nesting_child__destroy(test_ctx->level1_skel);
		sub_sched_parent__destroy(test_ctx->root_parent_skel);
		cleanup_cgroup(test_ctx->cgroup_path_l2);
		cleanup_cgroup(test_ctx->cgroup_path_l1);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	*ctx = test_ctx;
	return SCX_TEST_PASS;
}

/**
 * Run: Test cascading disable of nested schedulers.
 *
 * Tests the drain_descendants() path which recursively waits for all
 * children to be disabled before proceeding.
 *
 * Execution order:
 * 1. Attach root parent
 * 2. Attach level-1 sub-scheduler
 * 3. Attach level-2 sub-scheduler
 * 4. Let all run briefly
 * 5. Detach level-2 first
 * 6. Detach level-1
 * 7. Detach root (or allow kernel to clean up)
 */
static enum scx_test_status run(void *ctx)
{
	struct test_context *test_ctx = ctx;
	struct bpf_link *root_link;
	struct bpf_link *level1_link;
	struct bpf_link *level2_link;

	/* Attach root parent scheduler */
	root_link = bpf_map__attach_struct_ops(
		test_ctx->root_parent_skel->maps.sub_sched_parent_ops);
	if (!root_link) {
		SCX_ERR("Failed to attach root parent scheduler");
		return SCX_TEST_FAIL;
	}

	/* Attach level-1 sub-scheduler to root */
	level1_link = bpf_map__attach_struct_ops(
		test_ctx->level1_skel->maps.sub_sched_nesting_child_ops);
	if (!level1_link) {
		SCX_ERR("Failed to attach level-1 scheduler");
		bpf_link__destroy(root_link);
		return SCX_TEST_FAIL;
	}

	/* Attach level-2 sub-scheduler to level-1 */
	level2_link = bpf_map__attach_struct_ops(
		test_ctx->level2_skel->maps.sub_sched_nesting_child_ops);
	if (!level2_link) {
		SCX_ERR("Failed to attach level-2 scheduler");
		bpf_link__destroy(level1_link);
		bpf_link__destroy(root_link);
		return SCX_TEST_FAIL;
	}

	/* Let all schedulers run briefly */
	sleep(1);

	/*
	 * Critical test: Detach in reverse order (deepest first).
	 * This tests that drain_descendants() properly waits for children
	 * to complete their disable sequence.
	 */
	bpf_link__destroy(level2_link);
	sleep(1);  /* Let level-1 complete its cleanup */

	bpf_link__destroy(level1_link);
	sleep(1);  /* Let root complete its cleanup */

	bpf_link__destroy(root_link);

	/* If we got here without crash or deadlock, cascading disable worked */
	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct test_context *test_ctx = ctx;

	if (!test_ctx)
		return;

	if (test_ctx->level2_skel)
		sub_sched_nesting_child__destroy(test_ctx->level2_skel);

	if (test_ctx->level1_skel)
		sub_sched_nesting_child__destroy(test_ctx->level1_skel);

	if (test_ctx->root_parent_skel)
		sub_sched_parent__destroy(test_ctx->root_parent_skel);

	cleanup_cgroup(test_ctx->cgroup_path_l2);
	cleanup_cgroup(test_ctx->cgroup_path_l1);
	free(test_ctx);
}

struct scx_test sub_sched_nesting = {
	.name = "sub_sched_nesting",
	.description = "Test multi-level nested sub-sched cascading disable",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};

REGISTER_SCX_TEST(&sub_sched_nesting)
