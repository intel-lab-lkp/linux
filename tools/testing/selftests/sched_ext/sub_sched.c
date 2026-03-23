/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Basic test for sub-sched functionality.
 *
 * Tests the ability to attach a child scheduler to a specific cgroup
 * via the sub_cgroup_id mechanism.
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
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "sub_sched_parent.bpf.skel.h"
#include "sub_sched_child.bpf.skel.h"
#include "scx_test.h"

#define TEST_CGROUP_PATH "/sys/fs/cgroup/test_sub_sched"

struct test_context {
	struct sub_sched_parent *parent_skel;
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
 * Setup: Create cgroup, load both parent and child BPF programs.
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

	/* Set sub_cgroup_id to the test cgroup's inode */
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
 * Run: Test loading parent, then loading child scheduler.
 *
 * This tests:
 * 1. Parent scheduler loads successfully
 * 2. Child scheduler attaches to the specified cgroup
 * 3. No crashes or resource leaks
 */
static enum scx_test_status run(void *ctx)
{
	struct test_context *test_ctx = ctx;
	struct bpf_link *parent_link;
	struct bpf_link *child_link;

	/* Attach parent scheduler */
	parent_link = bpf_map__attach_struct_ops(test_ctx->parent_skel->maps.sub_sched_parent_ops);
	if (!parent_link) {
		SCX_ERR("Failed to attach parent scheduler");
		return SCX_TEST_FAIL;
	}

	/* Attach child scheduler to the cgroup */
	child_link = bpf_map__attach_struct_ops(test_ctx->child_skel->maps.sub_sched_child_ops);
	if (!child_link) {
		SCX_ERR("Failed to attach child scheduler");
		bpf_link__destroy(parent_link);
		return SCX_TEST_FAIL;
	}

	/* Let both schedulers run briefly to ensure they don't crash */
	sleep(1);

	/* Detach child first (sub-sched must be detached before parent) */
	bpf_link__destroy(child_link);

	/* Then detach parent */
	bpf_link__destroy(parent_link);

	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct test_context *test_ctx = ctx;

	if (!test_ctx)
		return;

	if (test_ctx->child_skel)
		sub_sched_child__destroy(test_ctx->child_skel);

	if (test_ctx->parent_skel)
		sub_sched_parent__destroy(test_ctx->parent_skel);

	cleanup_cgroup(test_ctx->cgroup_path);

	free(test_ctx);
}

struct scx_test sub_sched_basic = {
	.name = "sub_sched_basic",
	.description = "Test basic sub-sched attachment and detachment",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};

REGISTER_SCX_TEST(&sub_sched_basic)
