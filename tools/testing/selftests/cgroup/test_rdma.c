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

#ifdef HAVE_LIBIBVERBS
#include <infiniband/verbs.h>

static char *rdmacg_get_first_device(const char *cgroup)
{
	char buf[PAGE_SIZE];
	char *space;

	if (cg_read(cgroup, "rdma.max", buf, sizeof(buf)))
		return NULL;

	if (buf[0] == '\0')
		return NULL;

	space = strchr(buf, ' ');
	if (!space)
		return NULL;

	return strndup(buf, space - buf);
}

static long rdmacg_get_current_value(const char *cgroup, const char *device,
				     const char *resource)
{
	char buf[PAGE_SIZE];
	char pattern[256];
	char *p;

	if (cg_read(cgroup, "rdma.current", buf, sizeof(buf)))
		return -1;

	snprintf(pattern, sizeof(pattern), "%s ", device);
	p = strstr(buf, pattern);
	if (!p)
		return -1;

	snprintf(pattern, sizeof(pattern), "%s=", resource);
	p = strstr(p, pattern);
	if (!p)
		return -1;
	p += strlen(pattern);

	return strtol(p, NULL, 10);
}

static struct ibv_context *rdmacg_open_device(const char *device_name)
{
	struct ibv_device **dev_list;
	struct ibv_context *ctx = NULL;
	int i;

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list)
		return NULL;

	for (i = 0; dev_list[i]; i++) {
		if (!strcmp(ibv_get_device_name(dev_list[i]), device_name)) {
			ctx = ibv_open_device(dev_list[i]);
			break;
		}
	}
	ibv_free_device_list(dev_list);
	return ctx;
}

static int rdmacg_current_fn(const char *cgroup, void *arg)
{
	const char *device_name = (const char *)arg;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	long val;
	int ret = EXIT_FAILURE;

	ctx = rdmacg_open_device(device_name);
	if (!ctx)
		return EXIT_FAILURE;

	val = rdmacg_get_current_value(cgroup, device_name, "hca_handle");
	if (val != 1) {
		ksft_print_msg("hca_handle should be 1 after open, got %ld\n", val);
		goto cleanup;
	}
	val = rdmacg_get_current_value(cgroup, device_name, "hca_object");
	if (val != 0) {
		ksft_print_msg("hca_object should be 0 before alloc, got %ld\n", val);
		goto cleanup;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		ksft_print_msg("ibv_alloc_pd failed: %s\n", strerror(errno));
		goto cleanup;
	}
	val = rdmacg_get_current_value(cgroup, device_name, "hca_object");
	if (val != 1) {
		ksft_print_msg("hca_object should be 1 after alloc_pd, got %ld\n", val);
		goto cleanup;
	}

	/* After ibv_dealloc_pd: hca_object should be 0 */
	ibv_dealloc_pd(pd);
	pd = NULL;
	val = rdmacg_get_current_value(cgroup, device_name, "hca_object");
	if (val != 0) {
		ksft_print_msg("hca_object should be 0 after dealloc_pd, got %ld\n", val);
		goto cleanup;
	}

	/* After ibv_close_device: hca_handle should be 0 */
	ibv_close_device(ctx);
	ctx = NULL;
	val = rdmacg_get_current_value(cgroup, device_name, "hca_handle");
	if (val != 0) {
		ksft_print_msg("hca_handle should be 0 after close, got %ld\n", val);
		goto cleanup;
	}

	ret = EXIT_SUCCESS;

cleanup:
	if (pd)
		ibv_dealloc_pd(pd);
	if (ctx)
		ibv_close_device(ctx);
	return ret;
}

/*
 * Test: rdma.current responds to actual IB resource allocation and deallocation.
 */
static int test_rdmacg_current_response(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;
	char *device = NULL;

	cg = cg_name(root, "rdmacg_test_1");
	if (!cg)
		return KSFT_FAIL;

	if (cg_create(cg))
		goto cleanup;

	device = rdmacg_get_first_device(cg);
	if (!device) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	if (!cg_run(cg, rdmacg_current_fn, device))
		ret = KSFT_PASS;

cleanup:
	free(device);
	cg_destroy(cg);
	free(cg);
	return ret;
}

static int rdmacg_limit_fn(const char *cgroup, void *arg)
{
	const char *device_name = (const char *)arg;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd1 = NULL, *pd2 = NULL;
	int ret = EXIT_FAILURE;

	ctx = rdmacg_open_device(device_name);
	if (!ctx)
		return EXIT_FAILURE;

	/* First PD allocation should succeed (within hca_object=1 limit) */
	pd1 = ibv_alloc_pd(ctx);
	if (!pd1) {
		ksft_print_msg("first ibv_alloc_pd failed: %s\n", strerror(errno));
		goto cleanup;
	}

	/* Second PD allocation should fail (exceeds hca_object=1 limit) */
	pd2 = ibv_alloc_pd(ctx);
	if (pd2) {
		ksft_print_msg("second ibv_alloc_pd should have failed\n");
		goto cleanup;
	}

	/* Free first PD, then try again -- should succeed */
	ibv_dealloc_pd(pd1);
	pd1 = NULL;

	pd1 = ibv_alloc_pd(ctx);
	if (!pd1) {
		ksft_print_msg("ibv_alloc_pd after free failed: %s\n", strerror(errno));
		goto cleanup;
	}

	ret = EXIT_SUCCESS;

cleanup:
	if (pd1)
		ibv_dealloc_pd(pd1);
	if (pd2)
		ibv_dealloc_pd(pd2);
	if (ctx)
		ibv_close_device(ctx);
	return ret;
}

/*
 * Test: rdma.max limits are enforced -- exceeding hca_object limit causes
 * allocation failure.
 */
static int test_rdmacg_limit_enforcement(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;
	char *device = NULL;
	char buf[256];

	cg = cg_name(root, "rdmacg_test_2");
	if (!cg)
		return KSFT_FAIL;

	if (cg_create(cg))
		goto cleanup;

	device = rdmacg_get_first_device(cg);
	if (!device) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	snprintf(buf, sizeof(buf), "%s hca_handle=max hca_object=1", device);
	if (cg_write(cg, "rdma.max", buf)) {
		ksft_print_msg("failed to set hca_object=1 limit\n");
		goto cleanup;
	}

	if (!cg_run(cg, rdmacg_limit_fn, device))
		ret = KSFT_PASS;

cleanup:
	free(device);
	cg_destroy(cg);
	free(cg);
	return ret;
}

#define T(x) { x, #x }
struct rdmacg_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_rdmacg_current_response),
	T(test_rdmacg_limit_enforcement),
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

#else /* !HAVE_LIBIBVERBS */

int main(int argc, char **argv)
{
	ksft_print_header();
	ksft_exit_skip("test requires libibverbs\n");
}

#endif /* HAVE_LIBIBVERBS */
