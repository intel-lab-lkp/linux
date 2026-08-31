// SPDX-License-Identifier: GPL-2.0
/*
 * Test the dmem (device memory) cgroup controller.
 *
 * Depends on the dmem_selftest helper module
 * (tools/testing/selftests/cgroup/test_modules/dmem_selftest.c).
 */

#define _GNU_SOURCE

#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kselftest.h"
#include "cgroup_util.h"

#define DM_SELFTEST_REGION	"dmem_selftest"
#define DM_SELFTEST_ALLOC	"/sys/module/dmem_selftest/parameters/alloc"
#define DM_SELFTEST_FREE	"/sys/module/dmem_selftest/parameters/free"

static long dmem_read_limit(const char *cgroup, const char *ctrl)
{
	return cg_read_key_long(cgroup, ctrl, DM_SELFTEST_REGION " ");
}

static int dmem_write_limit(const char *cgroup, const char *ctrl, long val)
{
	char wr[64];

	snprintf(wr, sizeof(wr), "%s %ld", DM_SELFTEST_REGION, val);
	return cg_write(cgroup, ctrl, wr);
}

static int dmem_selftest_alloc(long bytes)
{
	char wr[32];

	snprintf(wr, sizeof(wr), "%ld", bytes);
	return write_text(DM_SELFTEST_ALLOC, wr, strlen(wr));
}

static int dmem_selftest_free(void)
{
	char wr[] = "1";

	return write_text(DM_SELFTEST_FREE, wr, strlen(wr));
}

/*
 * First, this test creates the following hierarchy:
 * A
 * A/B     dmem.max=8M
 * A/B/C   dmem.max=2M
 * A/B/D   dmem.max=1M
 * A/B/E   dmem.max=75K
 * A/B/F   dmem.max=25K
 * A/B/G   dmem.max=8K
 * A/B/H   dmem.max=0
 *
 * Then for each leaf cgroup it tries to alloc above dmem.max
 * and expects the request to fail and dmem.current to remain
 * unchanged.
 *
 * For leaves with non-zero dmem.max, it additionally allocs a
 * smaller amount and verifies dmem.current matches the requested
 * size exactly (the controller accounts in bytes, with no page
 * rounding), then frees and verifies dmem.current returns
 * to the previous value.
 */
static int test_dmem_max(const char *root)
{
	static const long leaf_max[] = {
		MB(2), MB(1), KB(75), KB(25), KB(8), 0
	};
	static const long pass_sz[] = {
		MB(1), MB(1), KB(4), KB(4), KB(4), 0
	};
	char *parent[2] = {NULL};
	char *children[ARRAY_SIZE(leaf_max)] = {NULL};

	_Static_assert(ARRAY_SIZE(pass_sz) == ARRAY_SIZE(leaf_max),
		       "pass_sz doesn't match leaf_max length");
	long cur_before, cur_after;
	int ret = KSFT_FAIL;
	int charged = 0;
	int in_child = 0;
	long v;
	int i;

	parent[0] = cg_name(root, "dmem_prot_0");
	if (!parent[0])
		goto cleanup;

	parent[1] = cg_name(parent[0], "dmem_prot_1");
	if (!parent[1])
		goto cleanup;

	if (cg_create(parent[0]))
		goto cleanup;

	if (cg_write(parent[0], "cgroup.subtree_control", "+dmem"))
		goto cleanup;

	if (cg_create(parent[1]))
		goto cleanup;

	if (cg_write(parent[1], "cgroup.subtree_control", "+dmem"))
		goto cleanup;

	for (i = 0; i < ARRAY_SIZE(children); i++) {
		children[i] = cg_name_indexed(parent[1], "dmem_child", i);
		if (!children[i])
			goto cleanup;
		if (cg_create(children[i]))
			goto cleanup;
	}

	if (dmem_write_limit(parent[1], "dmem.max", MB(8)))
		goto cleanup;
	for (i = 0; i < ARRAY_SIZE(children); i++)
		if (dmem_write_limit(children[i], "dmem.max", leaf_max[i]))
			goto cleanup;

	v = dmem_read_limit(parent[1], "dmem.max");
	if (v != MB(8))
		goto cleanup;
	for (i = 0; i < ARRAY_SIZE(children); i++) {
		v = dmem_read_limit(children[i], "dmem.max");
		if (v != leaf_max[i])
			goto cleanup;
	}

	for (i = 0; i < ARRAY_SIZE(children); i++) {
		if (cg_enter_current(children[i]))
			goto cleanup;
		in_child = 1;

		cur_before = dmem_read_limit(children[i], "dmem.current");
		if (cur_before < 0)
			goto cleanup;

		if (dmem_selftest_alloc(leaf_max[i] + 1) >= 0) {
			charged = 1;
			goto cleanup;
		}

		cur_after = dmem_read_limit(children[i], "dmem.current");
		if (cur_after != cur_before)
			goto cleanup;

		if (pass_sz[i] > 0) {
			if (dmem_selftest_alloc(pass_sz[i]) < 0)
				goto cleanup;
			charged = 1;

			cur_after = dmem_read_limit(children[i], "dmem.current");
			if (cur_after != cur_before + pass_sz[i])
				goto cleanup;

			if (dmem_selftest_free() < 0)
				goto cleanup;
			charged = 0;

			cur_after = dmem_read_limit(children[i], "dmem.current");
			if (cur_after != cur_before)
				goto cleanup;
		}

		if (cg_enter_current(root))
			goto cleanup;
		in_child = 0;
	}

	ret = KSFT_PASS;

cleanup:
	if (charged)
		dmem_selftest_free();
	if (in_child)
		cg_enter_current(root);
	for (i = ARRAY_SIZE(children) - 1; i >= 0; i--) {
		if (!children[i])
			continue;
		cg_destroy(children[i]);
		free(children[i]);
	}
	for (i = ARRAY_SIZE(parent) - 1; i >= 0; i--) {
		if (!parent[i])
			continue;
		cg_destroy(parent[i]);
		free(parent[i]);
	}
	return ret;
}

/*
 * Alloc non-page-aligned byte sizes and verify dmem.current matches
 * the requested size exactly. The controller charges the byte count
 * passed to dmem_cgroup_try_charge() with no page rounding, for this
 * helper and for production drivers. Then free must return usage to 0.
 */
static int test_dmem_alloc_byte_granularity(const char *root)
{
	static const long sizes[] = {
		1, 4095, 4097, KB(75) + 1, MB(1), MB(1) + 1
	};
	char *cg = NULL;
	long cur;
	int ret = KSFT_FAIL;
	int charged = 0;
	int in_child = 0;
	size_t i;

	cg = cg_name(root, "dmem_dbg_byte_gran");
	if (!cg)
		goto cleanup;

	if (cg_create(cg))
		goto cleanup;

	if (dmem_write_limit(cg, "dmem.max", MB(16)))
		goto cleanup;

	if (cg_enter_current(cg))
		goto cleanup;
	in_child = 1;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if (dmem_selftest_alloc(sizes[i]) < 0)
			goto cleanup;
		charged = 1;

		cur = dmem_read_limit(cg, "dmem.current");
		if (cur != sizes[i])
			goto cleanup;

		if (dmem_selftest_free() < 0)
			goto cleanup;
		charged = 0;

		cur = dmem_read_limit(cg, "dmem.current");
		if (cur != 0)
			goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	if (charged)
		dmem_selftest_free();
	if (in_child)
		cg_enter_current(root);
	if (cg) {
		cg_destroy(cg);
		free(cg);
	}
	return ret;
}

#define T(x) { x, #x }
struct dmem_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_dmem_max),
	T(test_dmem_alloc_byte_granularity),
};
#undef T

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	struct stat st;
	int i;

	ksft_print_header();

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	if (cg_read_strstr(root, "cgroup.controllers", "dmem"))
		ksft_exit_skip("dmem controller isn't available (CONFIG_CGROUP_DMEM?)\n");

	if (cg_read_strstr(root, "cgroup.subtree_control", "dmem"))
		if (cg_write(root, "cgroup.subtree_control", "+dmem"))
			ksft_exit_skip("Failed to enable dmem controller\n");

	if (stat(DM_SELFTEST_ALLOC, &st) < 0)
		ksft_exit_skip(
			"dmem_selftest helper not loaded (insmod test_modules/dmem_selftest.ko)\n");

	if (dmem_read_limit(root, "dmem.capacity") < 0)
		ksft_exit_skip("dmem_selftest region not registered\n");

	ksft_set_plan(ARRAY_SIZE(tests));

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	ksft_finished();
}
