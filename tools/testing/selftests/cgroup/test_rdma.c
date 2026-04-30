// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kselftest.h"
#include "cgroup_util.h"

static int rdmacg_read_max(const char *cgroup, char *buf, size_t len)
{
	return cg_read(cgroup, "rdma.max", buf, len);
}

static int rdmacg_write_max(const char *cgroup, const char *value)
{
	char *dup = strdup(value);
	int ret;

	if (!dup)
		return -1;
	ret = cg_write(cgroup, "rdma.max", dup);
	free(dup);
	return ret;
}

static char *rdmacg_get_first_device(const char *cgroup)
{
	char buf[PAGE_SIZE];
	char *space;

	if (rdmacg_read_max(cgroup, buf, sizeof(buf)))
		return NULL;

	if (buf[0] == '\0')
		return NULL;

	space = strchr(buf, ' ');
	if (!space)
		return NULL;

	return strndup(buf, space - buf);
}

/*
 * Check that a specific resource value for a device in rdma.max matches
 * the expected string. Returns 0 on match, -1 otherwise.
 */
static int rdmacg_check_value(const char *cgroup, const char *device,
			      const char *resource, const char *expected)
{
	char buf[PAGE_SIZE];
	char pattern[256];
	char *p;
	size_t elen;

	if (rdmacg_read_max(cgroup, buf, sizeof(buf)))
		return -1;

	/* Find device line */
	snprintf(pattern, sizeof(pattern), "%s ", device);
	p = strstr(buf, pattern);
	if (!p)
		return -1;

	/* Find resource= within this line */
	snprintf(pattern, sizeof(pattern), "%s=", resource);
	p = strstr(p, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);

	elen = strlen(expected);
	if (strncmp(p, expected, elen))
		return -1;

	if (p[elen] != ' ' && p[elen] != '\n' && p[elen] != '\0')
		return -1;

	return 0;
}

/*
 * Test: rdma.max file exists and is readable on child cgroup.
 */
static int test_rdmacg_max_read(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;
	char buf[PAGE_SIZE];

	cg = cg_name(root, "rdmacg_test_1");
	if (!cg)
		return KSFT_FAIL;

	if (cg_create(cg))
		goto cleanup;

	/* rdma.max should be readable on non-root cgroup */
	if (rdmacg_read_max(cg, buf, sizeof(buf)))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * Test: Writing a non-existent device name to rdma.max should fail.
 * The kernel returns -ENOENT when the device is not registered.
 */
static int test_rdmacg_max_write_nonexistent(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	cg = cg_name(root, "rdmacg_test_2");
	if (!cg)
		return KSFT_FAIL;

	if (cg_create(cg))
		goto cleanup;

	/* Write with a device that does not exist */
	if (rdmacg_write_max(cg, "fake_device_xyz hca_handle=10") == 0)
		goto cleanup;

	/* File should still be readable */
	char buf[PAGE_SIZE];
	if (rdmacg_read_max(cg, buf, sizeof(buf)))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * Test: Writing invalid format to rdma.max should fail.
 */
static int test_rdmacg_max_write_invalid(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	cg = cg_name(root, "rdmacg_test_3");
	if (!cg)
		return KSFT_FAIL;

	if (cg_create(cg))
		goto cleanup;

	/* Bare number -- not a valid device entry */
	if (rdmacg_write_max(cg, "42") == 0)
		goto cleanup;

	/* Missing value after '=' */
	if (rdmacg_write_max(cg, "fake_dev hca_handle=") == 0)
		goto cleanup;

	/* Negative value */
	if (rdmacg_write_max(cg, "fake_dev hca_handle=-1") == 0)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * Test: Set and read back limits when an RDMA device is present.
 * Skipped if no RDMA device is registered.
 */
static int test_rdmacg_max_device_limits(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;
	char *device = NULL;
	char buf[256];

	cg = cg_name(root, "rdmacg_test_4");
	if (!cg)
		return KSFT_FAIL;

	if (cg_create(cg))
		goto cleanup;

	device = rdmacg_get_first_device(cg);
	if (!device) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	snprintf(buf, sizeof(buf), "%s hca_handle=5 hca_object=100", device);
	if (rdmacg_write_max(cg, buf))
		goto cleanup;

	if (rdmacg_check_value(cg, device, "hca_handle", "5"))
		goto cleanup;
	if (rdmacg_check_value(cg, device, "hca_object", "100"))
		goto cleanup;

	snprintf(buf, sizeof(buf), "%s hca_handle=20", device);
	if (rdmacg_write_max(cg, buf))
		goto cleanup;

	if (rdmacg_check_value(cg, device, "hca_handle", "20"))
		goto cleanup;
	if (rdmacg_check_value(cg, device, "hca_object", "100"))
		goto cleanup;

	snprintf(buf, sizeof(buf), "%s hca_handle=max hca_object=max", device);
	if (rdmacg_write_max(cg, buf))
		goto cleanup;

	if (rdmacg_check_value(cg, device, "hca_handle", "max"))
		goto cleanup;
	if (rdmacg_check_value(cg, device, "hca_object", "max"))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	free(device);
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * Test: Hierarchy -- parent limits and child limits are independent.
 * The child defaults to "max"; parent limits constrain at charge time.
 */
static int test_rdmacg_max_hierarchy(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg_parent = NULL, *cg_child = NULL;
	char *device = NULL;
	char buf[256];

	cg_parent = cg_name(root, "rdmacg_parent");
	cg_child = cg_name(cg_parent, "rdmacg_child");
	if (!cg_parent || !cg_child)
		goto cleanup;

	if (cg_create(cg_parent))
		goto cleanup;

	if (cg_write(cg_parent, "cgroup.subtree_control", "+rdma"))
		goto cleanup;

	if (cg_create(cg_child))
		goto cleanup;

	device = rdmacg_get_first_device(cg_child);
	if (!device) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	snprintf(buf, sizeof(buf), "%s hca_handle=10 hca_object=200", device);
	if (rdmacg_write_max(cg_parent, buf))
		goto cleanup;

	if (rdmacg_check_value(cg_parent, device, "hca_handle", "10"))
		goto cleanup;

	if (rdmacg_check_value(cg_child, device, "hca_handle", "max"))
		goto cleanup;

	snprintf(buf, sizeof(buf), "%s hca_handle=3 hca_object=50", device);
	if (rdmacg_write_max(cg_child, buf))
		goto cleanup;

	if (rdmacg_check_value(cg_child, device, "hca_handle", "3"))
		goto cleanup;
	if (rdmacg_check_value(cg_child, device, "hca_object", "50"))
		goto cleanup;

	if (rdmacg_check_value(cg_parent, device, "hca_handle", "10"))
		goto cleanup;
	if (rdmacg_check_value(cg_parent, device, "hca_object", "200"))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	free(device);
	if (cg_child)
		cg_destroy(cg_child);
	if (cg_parent)
		cg_destroy(cg_parent);
	free(cg_child);
	free(cg_parent);
	return ret;
}

#define T(x) { x, #x }
struct rdmacg_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_rdmacg_max_read),
	T(test_rdmacg_max_write_nonexistent),
	T(test_rdmacg_max_write_invalid),
	T(test_rdmacg_max_device_limits),
	T(test_rdmacg_max_hierarchy),
};
#undef T

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	char orig_subtree[PAGE_SIZE] = {0};
	bool rdma_was_enabled = false;

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(tests));

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	if (cg_read_strstr(root, "cgroup.controllers", "rdma"))
		ksft_exit_skip("rdma controller isn't available\n");

	/* Save original subtree_control so we can restore it later */
	if (cg_read(root, "cgroup.subtree_control", orig_subtree,
		    sizeof(orig_subtree)))
		orig_subtree[0] = '\0';

	rdma_was_enabled = (strstr(orig_subtree, "rdma") != NULL);

	/* Enable rdma controller if not already enabled */
	if (!rdma_was_enabled) {
		if (cg_write(root, "cgroup.subtree_control", "+rdma"))
			ksft_exit_skip("Failed to enable rdma controller\n");
	}

	for (int i = 0; i < ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	/* Restore original subtree_control state */
	if (!rdma_was_enabled)
		cg_write(root, "cgroup.subtree_control", "-rdma");

	ksft_finished();
}
