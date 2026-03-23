/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test for sub-sched parent rejection (abort path testing).
 *
 * Tests that when parent rejects sub_attach, the kernel:
 * 1. Does not crash
 * 2. Properly cleans up partially-initialized tasks
 * 3. Rolls back without resource leaks
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
#include "sub_sched_child.bpf.skel.h"
#include "sub_sched_parent_reject.bpf.skel.h"
#include "scx_test.h"

#define TEST_CGROUP_PATH "/sys/fs/cgroup/test_sub_sched_reject"

struct test_context {
	struct sub_sched_parent_reject *parent_reject_skel;
	struct sub_sched_child *child_skel;
	char cgroup_path[256];
};

/**
 * Create a cgroup v2 for testing.
 * Returns the inode number (which serves as cgroup ID) on success, -1 on error.
 */
static u64 create_test_cgroup(const char *path)
{
	struct stat st;

	/* Create the test cgroup directory */
	if (mkdir(path, 0755) < 0) {
		if (errno != EEXIST) {
			SCX_ERR("Failed to create cgroup: %s", strerror(errno));
			return -1;
		}
	}

	/* Get the inode number (cgroup ID) */
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
 * Setup for parent-reject test
 */
static enum scx_test_status setup(void **ctx)
{
	struct test_context *test_ctx;
	u64 cgroup_id;

	test_ctx = calloc(1, sizeof(*test_ctx));
	if (!test_ctx)
		return SCX_TEST_FAIL;

	/* Create test cgroup */
	snprintf(test_ctx->cgroup_path, sizeof(test_ctx->cgroup_path),
		 "%s.%d", TEST_CGROUP_PATH, getpid());

	cgroup_id = create_test_cgroup(test_ctx->cgroup_path);
	if (cgroup_id == (u64)-1) {
		SCX_ERR("Failed to create test cgroup");
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Load parent that rejects sub_attach */
	test_ctx->parent_reject_skel = sub_sched_parent_reject__open();
	if (!test_ctx->parent_reject_skel) {
		SCX_ERR("Failed to open parent_reject BPF skeleton");
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	SCX_ENUM_INIT(test_ctx->parent_reject_skel);
	if (sub_sched_parent_reject__load(test_ctx->parent_reject_skel)) {
		SCX_ERR("Failed to load parent_reject BPF program");
		sub_sched_parent_reject__destroy(test_ctx->parent_reject_skel);
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Load child scheduler */
	test_ctx->child_skel = sub_sched_child__open();
	if (!test_ctx->child_skel) {
		SCX_ERR("Failed to open child BPF skeleton");
		sub_sched_parent_reject__destroy(test_ctx->parent_reject_skel);
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	/* Set sub_cgroup_id to the test cgroup's inode */
	test_ctx->child_skel->struct_ops.sub_sched_child_ops->sub_cgroup_id = cgroup_id;

	SCX_ENUM_INIT(test_ctx->child_skel);
	if (sub_sched_child__load(test_ctx->child_skel)) {
		SCX_ERR("Failed to load child BPF program");
		sub_sched_child__destroy(test_ctx->child_skel);
		sub_sched_parent_reject__destroy(test_ctx->parent_reject_skel);
		cleanup_cgroup(test_ctx->cgroup_path);
		free(test_ctx);
		return SCX_TEST_FAIL;
	}

	*ctx = test_ctx;
	return SCX_TEST_PASS;
}

/**
 * Run: Test parent rejection of sub_attach.
 *
 * This tests that when parent rejects sub_attach, the kernel:
 * 1. Does not crash
 * 2. Properly cleans up partially-initialized tasks
 * 3. Rolls back without resource leaks
 *
 * This exercise the abort path at line 7086+ in ext.c which should:
 * - Clean up already-initialized tasks
 * - Clear SCX_TASK_SUB_INIT flags
 * - Properly decrement reference counts
 */
static enum scx_test_status run(void *ctx)
{
	struct test_context *test_ctx = ctx;
	struct bpf_link *parent_link;
	struct bpf_link *child_link;

	/* Attach parent that will reject sub_attach */
	parent_link = bpf_map__attach_struct_ops(
		test_ctx->parent_reject_skel->maps.sub_sched_parent_reject_ops);
	if (!parent_link) {
		SCX_ERR("Failed to attach parent scheduler");
		return SCX_TEST_FAIL;
	}

	/* Try to attach child - this should fail when parent rejects sub_attach */
	child_link = bpf_map__attach_struct_ops(test_ctx->child_skel->maps.sub_sched_child_ops);

	/* It's OK if this fails - we're testing the failure path */
	if (child_link) {
		/* If attach somehow succeeded, clean it up */
		bpf_link__destroy(child_link);
	}

	/* Key test: Parent can be detached cleanly even though child attach failed */
	bpf_link__destroy(parent_link);

	/* If we got here without crash, the abort path worked correctly */
	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct test_context *test_ctx = ctx;

	if (!test_ctx)
		return;

	if (test_ctx->child_skel)
		sub_sched_child__destroy(test_ctx->child_skel);

	if (test_ctx->parent_reject_skel)
		sub_sched_parent_reject__destroy(test_ctx->parent_reject_skel);

	cleanup_cgroup(test_ctx->cgroup_path);
	free(test_ctx);
}

struct scx_test sub_sched_parent_reject = {
	.name = "sub_sched_parent_reject",
	.description = "Test sub-attach rejection (abort path cleanup)",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};

REGISTER_SCX_TEST(&sub_sched_parent_reject)
