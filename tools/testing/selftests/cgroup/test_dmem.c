// SPDX-License-Identifier: GPL-2.0
/*
 * Test the dmem (device memory) cgroup controller.
 *
 * Depends on dmem_selftest kernel module.
 */

#define _GNU_SOURCE

#include <linux/limits.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kselftest.h"
#include "cgroup_util.h"

/* kernel/cgroup/dmem_selftest.c */
#define DM_SELFTEST_REGION	"dmem_selftest"
#define DM_SELFTEST_CHARGE	"/sys/kernel/debug/dmem_selftest/charge"
#define DM_SELFTEST_UNCHARGE	"/sys/kernel/debug/dmem_selftest/uncharge"

/*
 * Parse the first line of dmem.capacity (root):
 *   "<name> <size_in_bytes>"
 * Returns 1 if a region was found, 0 if capacity is empty, -1 on read error.
 */
static int parse_first_region(const char *root, char *name, size_t name_len,
			      unsigned long long *size_out)
{
	char buf[4096];
	char nm[256];
	unsigned long long sz;

	if (cg_read(root, "dmem.capacity", buf, sizeof(buf)) < 0)
		return -1;

	if (sscanf(buf, "%255s %llu", nm, &sz) < 2)
		return 0;

	if (name_len <= strlen(nm))
		return -1;

	strcpy(name, nm);
	*size_out = sz;
	return 1;
}

/*
 * Read the numeric limit for @region_name from a multiline
 * dmem.{min,low,max} file. Returns bytes,
 * or -1 if the line is "<name> max", or -2 if missing/err.
 */
static long long dmem_read_limit_for_region(const char *cgroup, const char *ctrl,
					    const char *region_name)
{
	char buf[4096];
	char *line, *saveptr = NULL;
	char fname[256];
	char fval[64];

	if (cg_read(cgroup, ctrl, buf, sizeof(buf)) < 0)
		return -2;

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		if (!line[0])
			continue;
		if (sscanf(line, "%255s %63s", fname, fval) != 2)
			continue;
		if (strcmp(fname, region_name))
			continue;
		if (!strcmp(fval, "max"))
			return -1;
		return strtoll(fval, NULL, 0);
	}
	return -2;
}

static long long dmem_read_limit(const char *cgroup, const char *ctrl)
{
	return dmem_read_limit_for_region(cgroup, ctrl, DM_SELFTEST_REGION);
}

static int dmem_write_limit(const char *cgroup, const char *ctrl,
			    const char *val)
{
	char wr[512];

	snprintf(wr, sizeof(wr), "%s %s", DM_SELFTEST_REGION, val);
	return cg_write(cgroup, ctrl, wr);
}

static int dmem_selftest_charge_bytes(unsigned long long bytes)
{
	char wr[32];

	snprintf(wr, sizeof(wr), "%llu", bytes);
	return write_text(DM_SELFTEST_CHARGE, wr, strlen(wr));
}

static int dmem_selftest_uncharge(void)
{
	return write_text(DM_SELFTEST_UNCHARGE, "\n", 1);
}

/*
 * First, this test creates the following hierarchy:
 * A
 * A/B     dmem.max=1M
 * A/B/C   dmem.max=75K
 * A/B/D   dmem.max=25K
 * A/B/E   dmem.max=8K
 * A/B/F   dmem.max=0
 *
 * Then for each leaf cgroup it tries to charge above dmem.max
 * and expects the charge request to fail and dmem.current to
 * remain unchanged.
 *
 * For leaves with non-zero dmem.max, it additionally charges a
 * smaller amount and verifies accounting grows within one PAGE_SIZE
 * rounding bound, then uncharges and verifies dmem.current returns
 * to the previous value.
 *
 */
static int test_dmem_max(const char *root)
{
	static const char * const leaf_max[] = { "75K", "25K", "8K", "0" };
	static const unsigned long long fail_sz[] = {
		(75ULL * 1024ULL) + 1ULL,
		(25ULL * 1024ULL) + 1ULL,
		(8ULL * 1024ULL) + 1ULL,
		1ULL
	};
	static const unsigned long long pass_sz[] = {
		4096ULL, 4096ULL, 4096ULL, 0ULL
	};
	char *parent[2] = {NULL};
	char *children[4] = {NULL};
	unsigned long long cap;
	char region[256];
	long long page_size;
	long long cur_before, cur_after;
	int ret = KSFT_FAIL;
	int charged = 0;
	int in_child = 0;
	long long v;
	int i;

	if (access(DM_SELFTEST_CHARGE, W_OK) != 0)
		return KSFT_SKIP;

	if (parse_first_region(root, region, sizeof(region), &cap) != 1)
		return KSFT_SKIP;
	if (strcmp(region, DM_SELFTEST_REGION) != 0)
		return KSFT_SKIP;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		goto cleanup;

	parent[0] = cg_name(root, "dmem_prot_0");
	parent[1] = cg_name(parent[0], "dmem_prot_1");
	if (!parent[0] || !parent[1])
		goto cleanup;

	if (cg_create(parent[0]))
		goto cleanup;

	if (cg_write(parent[0], "cgroup.subtree_control", "+dmem"))
		goto cleanup;

	if (cg_create(parent[1]))
		goto cleanup;

	if (cg_write(parent[1], "cgroup.subtree_control", "+dmem"))
		goto cleanup;

	for (i = 0; i < 4; i++) {
		children[i] = cg_name_indexed(parent[1], "dmem_child", i);
		if (!children[i])
			goto cleanup;
		if (cg_create(children[i]))
			goto cleanup;
	}

	if (dmem_write_limit(parent[1], "dmem.max", "1M"))
		goto cleanup;
	for (i = 0; i < 4; i++)
		if (dmem_write_limit(children[i], "dmem.max", leaf_max[i]))
			goto cleanup;

	v = dmem_read_limit(parent[1], "dmem.max");
	if (!values_close(v, 1024LL * 1024LL, 3))
		goto cleanup;
	v = dmem_read_limit(children[0], "dmem.max");
	if (!values_close(v, 75LL * 1024LL, 3))
		goto cleanup;
	v = dmem_read_limit(children[1], "dmem.max");
	if (!values_close(v, 25LL * 1024LL, 3))
		goto cleanup;
	v = dmem_read_limit(children[2], "dmem.max");
	if (!values_close(v, 8LL * 1024LL, 3))
		goto cleanup;
	v = dmem_read_limit(children[3], "dmem.max");
	if (v != 0)
		goto cleanup;

	for (i = 0; i < 4; i++) {
		if (cg_enter_current(children[i]))
			goto cleanup;
		in_child = 1;

		cur_before = dmem_read_limit(children[i], "dmem.current");
		if (cur_before < 0)
			goto cleanup;

		if (dmem_selftest_charge_bytes(fail_sz[i]) == 0)
			goto cleanup;

		cur_after = dmem_read_limit(children[i], "dmem.current");
		if (cur_after != cur_before)
			goto cleanup;

		if (pass_sz[i] > 0) {
			if (dmem_selftest_charge_bytes(pass_sz[i]) < 0)
				goto cleanup;
			charged = 1;

			cur_after = dmem_read_limit(children[i], "dmem.current");
			if (cur_after < cur_before + (long long)pass_sz[i])
				goto cleanup;
			if (cur_after > cur_before + (long long)pass_sz[i] + page_size)
				goto cleanup;

			if (dmem_selftest_uncharge() < 0)
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
		dmem_selftest_uncharge();
	if (in_child)
		cg_enter_current(root);
	for (i = 3; i >= 0; i--) {
		if (!children[i])
			continue;
		cg_destroy(children[i]);
		free(children[i]);
	}
	for (i = 1; i >= 0; i--) {
		if (!parent[i])
			continue;
		cg_destroy(parent[i]);
		free(parent[i]);
	}
	return ret;
}

/*
 * This test sets dmem.min and dmem.low on a child cgroup, then charge
 * from that context and verify dmem.current tracks the charged bytes
 * (within one page rounding).
 */
static int test_dmem_charge_with_attr(const char *root, bool min)
{
	char region[256];
	unsigned long long cap;
	const unsigned long long charge_sz = 12345ULL;
	const char *attribute = min ? "dmem.min" : "dmem.low";
	int ret = KSFT_FAIL;
	char *cg = NULL;
	long long cur;
	long long page_size;
	int charged = 0;
	int in_child = 0;

	if (access(DM_SELFTEST_CHARGE, W_OK) != 0)
		return KSFT_SKIP;

	if (parse_first_region(root, region, sizeof(region), &cap) != 1)
		return KSFT_SKIP;
	if (strcmp(region, DM_SELFTEST_REGION) != 0)
		return KSFT_SKIP;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		goto cleanup;

	cg = cg_name(root, "test_dmem_attr");
	if (!cg)
		goto cleanup;

	if (cg_create(cg))
		goto cleanup;

	if (cg_enter_current(cg))
		goto cleanup;
	in_child = 1;

	if (dmem_write_limit(cg, attribute, "16K"))
		goto cleanup;

	if (dmem_selftest_charge_bytes(charge_sz) < 0)
		goto cleanup;
	charged = 1;

	cur = dmem_read_limit(cg, "dmem.current");
	if (cur < (long long)charge_sz)
		goto cleanup;
	if (cur > (long long)charge_sz + page_size)
		goto cleanup;

	if (dmem_selftest_uncharge() < 0)
		goto cleanup;
	charged = 0;

	cur = dmem_read_limit(cg, "dmem.current");
	if (cur != 0)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	if (charged)
		dmem_selftest_uncharge();
	if (in_child)
		cg_enter_current(root);
	cg_destroy(cg);
	free(cg);
	return ret;
}

static int test_dmem_min(const char *root)
{
	return test_dmem_charge_with_attr(root, "dmem.min");
}

static int test_dmem_low(const char *root)
{
	return test_dmem_charge_with_attr(root, "dmem.low");
}

/*
 * This test charges non-page-aligned byte sizes and verify dmem.current
 * stays consistent: it must account at least the requested bytes and
 * never exceed one kernel page of rounding overhead. Then uncharge must
 * return usage to 0.
 */
static int test_dmem_charge_byte_granularity(const char *root)
{
	static const unsigned long long sizes[] = { 1ULL, 4095ULL, 4097ULL, 12345ULL };
	char *cg = NULL;
	unsigned long long cap;
	char region[256];
	long long cur;
	long long page_size;
	int ret = KSFT_FAIL;
	int charged = 0;
	int in_child = 0;
	size_t i;

	if (access(DM_SELFTEST_CHARGE, W_OK) != 0)
		return KSFT_SKIP;

	if (parse_first_region(root, region, sizeof(region), &cap) != 1)
		return KSFT_SKIP;
	if (strcmp(region, DM_SELFTEST_REGION) != 0)
		return KSFT_SKIP;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		goto cleanup;

	cg = cg_name(root, "dmem_dbg_byte_gran");
	if (!cg)
		goto cleanup;

	if (cg_create(cg))
		goto cleanup;

	if (dmem_write_limit(cg, "dmem.max", "8M"))
		goto cleanup;

	if (cg_enter_current(cg))
		goto cleanup;
	in_child = 1;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if (dmem_selftest_charge_bytes(sizes[i]) < 0)
			goto cleanup;
		charged = 1;

		cur = dmem_read_limit(cg, "dmem.current");
		if (cur < (long long)sizes[i])
			goto cleanup;
		if (cur > (long long)sizes[i] + page_size)
			goto cleanup;

		if (dmem_selftest_uncharge() < 0)
			goto cleanup;
		charged = 0;

		cur = dmem_read_limit(cg, "dmem.current");
		if (cur != 0)
			goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	if (charged)
		dmem_selftest_uncharge();
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
	T(test_dmem_min),
	T(test_dmem_low),
	T(test_dmem_charge_byte_granularity),
};
#undef T

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	int i;

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(tests));

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	if (cg_read_strstr(root, "cgroup.controllers", "dmem"))
		ksft_exit_skip("dmem controller isn't available (CONFIG_CGROUP_DMEM?)\n");

	if (cg_read_strstr(root, "cgroup.subtree_control", "dmem"))
		if (cg_write(root, "cgroup.subtree_control", "+dmem"))
			ksft_exit_skip("Failed to enable dmem controller\n");

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip(
				"%s (need CONFIG_DMEM_SELFTEST, modprobe dmem_selftest)\n",
				tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	ksft_finished();
}
