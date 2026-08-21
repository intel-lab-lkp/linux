// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <linux/limits.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include "kselftest.h"
#include "cgroup_util.h"

#define CMA_HEAP_DEFAULT_PATH	"/dev/dma_heap/default_cma_region"
#define CMA_RESERVED_CURRENT	"memory.cma.reserved.current"
#define CMA_RESERVED_MAX	"memory.cma.reserved.max"

#define HUGETLB_CMA_CURRENT	"memory.cma.hugetlb0.current"
#define HUGETLB_CMA_MAX	"memory.cma.hugetlb0.max"
#define HUGEPAGES_DIR		"/sys/kernel/mm/hugepages"

static int cma_heap_alloc(int heap_fd, size_t size)
{
	struct dma_heap_allocation_data data = {
		.len = size,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
		return -1;

	return data.fd;
}

static int test_cma_charge(const char *root)
{
	int heap_fd = -1, buf_fd = -1;
	int rc = KSFT_FAIL, in_cg = 0;
	size_t size = 4UL << 20;
	long before, after;
	char *cg;

	cg = cg_name(root, "memcg_cma_charge");
	if (!cg)
		goto cleanup;

	if (cg_create(cg))
		goto cleanup;

	if (cg_enter_current(cg))
		goto cleanup;
	in_cg = 1;

	heap_fd = open(CMA_HEAP_DEFAULT_PATH, O_RDONLY | O_CLOEXEC);
	if (heap_fd < 0) {
		ksft_print_msg("cannot open CMA heap (%m)\n");
		goto cleanup;
	}

	before = cg_read_long(cg, CMA_RESERVED_CURRENT);

	buf_fd = cma_heap_alloc(heap_fd, size);
	if (buf_fd < 0) {
		ksft_print_msg("cannot allocate in CMA heap (%m)\n");
		goto cleanup;
	}

	after = cg_read_long(cg, CMA_RESERVED_CURRENT);

	if (after - before < (long)size) {
		ksft_print_msg(
			"invalid CMA counter after charge: before=%ld after=%ld\n",
			before, after);
		goto cleanup;
	}

	close(buf_fd);
	buf_fd = -1;

	after = cg_read_long(cg, CMA_RESERVED_CURRENT);
	if (!values_close(before, after, 3)) {
		ksft_print_msg(
			"invalid CMA counter after uncharge: before=%ld after=%ld\n",
			before, after);
		goto cleanup;
	}

	rc = KSFT_PASS;

cleanup:
	if (buf_fd >= 0)
		close(buf_fd);
	if (heap_fd >= 0)
		close(heap_fd);

	if (in_cg)
		cg_enter_current(root);
	cg_destroy(cg);
	free(cg);

	return rc;
}

static int test_cma_limit(const char *root)
{
	int heap_fd = -1, buf_fd = -1;
	int rc = KSFT_FAIL, in_cg = 0;
	size_t size = 4UL << 20;
	char *cg;

	cg = cg_name(root, "memcg_cma_limit");
	if (!cg)
		goto cleanup;

	if (cg_create(cg))
		goto cleanup;

	if (cg_enter_current(cg))
		goto cleanup;
	in_cg = 1;

	if (cg_write(cg, CMA_RESERVED_MAX, "2M")) {
		ksft_print_msg("cannot set CMA limit (%m)\n");
		goto cleanup;
	}

	heap_fd = open(CMA_HEAP_DEFAULT_PATH, O_RDONLY | O_CLOEXEC);
	if (heap_fd < 0) {
		ksft_print_msg("cannot open CMA heap (%m)\n");
		goto cleanup;
	}

	buf_fd = cma_heap_alloc(heap_fd, size);
	if (buf_fd >= 0) {
		ksft_print_msg("CMA limit not enforced.\n");
		close(buf_fd);
	} else
		rc = KSFT_PASS;

cleanup:
	if (heap_fd >= 0)
		close(heap_fd);

	if (in_cg)
		cg_enter_current(root);
	cg_destroy(cg);
	free(cg);

	return rc;
}

static long find_largest_hugepage_size_kb(void)
{
	DIR *dir;
	struct dirent *entry;
	long max_size = 0;

	dir = opendir(HUGEPAGES_DIR);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		long size;

		if (sscanf(entry->d_name, "hugepages-%ldkB", &size) == 1) {
			if (size > max_size)
				max_size = size;
		}
	}
	closedir(dir);

	return max_size > 0 ? max_size : -1;
}

static long read_sysfs_long(const char *path)
{
	FILE *f;
	long val = -1;

	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%ld", &val) != 1)
		val = -1;
	fclose(f);
	return val;
}

static int write_sysfs_long(const char *path, long val)
{
	FILE *f;
	int rc;

	f = fopen(path, "w");
	if (!f)
		return -1;
	rc = fprintf(f, "%ld", val);
	fclose(f);
	return rc < 0 ? -1 : 0;
}

static int test_hugetlb_cma_charge(const char *root)
{
	int rc = KSFT_SKIP, in_cg = 0;
	long hpage_size_kb, hpage_size_bytes;
	long orig_nr = -1, new_nr, before, after;
	char nr_path[PATH_MAX];
	char *cg;

	hpage_size_kb = find_largest_hugepage_size_kb();
	if (hpage_size_kb < 0) {
		ksft_print_msg("no hugepage size found\n");
		return KSFT_SKIP;
	}
	hpage_size_bytes = hpage_size_kb * 1024;

	snprintf(nr_path, sizeof(nr_path),
		 HUGEPAGES_DIR "/hugepages-%ldkB/nr_hugepages", hpage_size_kb);

	cg = cg_name(root, "memcg_hugetlb_cma_charge");
	if (!cg)
		goto cleanup;

	if (cg_create(cg))
		goto cleanup;

	if (cg_enter_current(cg))
		goto cleanup;
	in_cg = 1;

	if (cg_read_long(cg, HUGETLB_CMA_CURRENT) < 0) {
		ksft_print_msg("hugetlb CMA counters not available\n");
		goto cleanup;
	}

	orig_nr = read_sysfs_long(nr_path);
	if (orig_nr < 0) {
		ksft_print_msg("cannot read %s\n", nr_path);
		goto cleanup;
	}

	before = cg_read_long(cg, HUGETLB_CMA_CURRENT);

	if (write_sysfs_long(nr_path, orig_nr + 1)) {
		ksft_print_msg("cannot write %s\n", nr_path);
		goto cleanup;
	}

	new_nr = read_sysfs_long(nr_path);
	if (new_nr <= orig_nr) {
		ksft_print_msg("failed to allocate a hugepage\n");
		goto cleanup;
	}

	rc = KSFT_FAIL;
	after = cg_read_long(cg, HUGETLB_CMA_CURRENT);

	if (after - before < hpage_size_bytes) {
		ksft_print_msg(
			"hugetlb CMA counter not charged: before=%ld after=%ld expected +%ld\n",
			before, after, hpage_size_bytes);
		goto cleanup;
	}

	write_sysfs_long(nr_path, orig_nr);

	after = cg_read_long(cg, HUGETLB_CMA_CURRENT);
	if (!values_close(before, after, 3)) {
		ksft_print_msg(
			"hugetlb CMA counter not uncharged: before=%ld after=%ld\n",
			before, after);
		goto cleanup;
	}

	rc = KSFT_PASS;

cleanup:
	if (orig_nr >= 0)
		write_sysfs_long(nr_path, orig_nr);

	if (in_cg)
		cg_enter_current(root);
	cg_destroy(cg);
	free(cg);

	return rc;
}

static int test_hugetlb_cma_limit(const char *root)
{
	int rc = KSFT_SKIP, in_cg = 0;
	long hpage_size_kb;
	long orig_nr, new_nr;
	char nr_path[PATH_MAX];
	char *cg;

	hpage_size_kb = find_largest_hugepage_size_kb();
	if (hpage_size_kb < 0) {
		ksft_print_msg("no hugepage size found\n");
		return KSFT_SKIP;
	}

	snprintf(nr_path, sizeof(nr_path),
		 HUGEPAGES_DIR "/hugepages-%ldkB/nr_hugepages", hpage_size_kb);

	cg = cg_name(root, "memcg_hugetlb_cma_limit");
	if (!cg)
		goto cleanup;

	if (cg_create(cg))
		goto cleanup;

	if (cg_enter_current(cg))
		goto cleanup;
	in_cg = 1;

	if (cg_read_long(cg, HUGETLB_CMA_MAX) < 0) {
		ksft_print_msg("hugetlb CMA counters not available\n");
		goto cleanup;
	}

	if (cg_write(cg, HUGETLB_CMA_MAX, "0")) {
		ksft_print_msg("cannot set hugetlb CMA limit\n");
		goto cleanup;
	}

	orig_nr = read_sysfs_long(nr_path);
	if (orig_nr < 0) {
		ksft_print_msg("cannot read %s\n", nr_path);
		goto cleanup;
	}

	write_sysfs_long(nr_path, orig_nr + 1);
	new_nr = read_sysfs_long(nr_path);

	rc = KSFT_FAIL;
	if (new_nr > orig_nr) {
		ksft_print_msg("hugetlb CMA limit not enforced\n");
		write_sysfs_long(nr_path, orig_nr);
	} else {
		rc = KSFT_PASS;
	}

cleanup:
	if (in_cg)
		cg_enter_current(root);
	cg_destroy(cg);
	free(cg);

	return rc;
}

#define T(x) { x, #x }
struct cma_memcg_tests {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_cma_charge),
	T(test_cma_limit),
	T(test_hugetlb_cma_charge),
	T(test_hugetlb_cma_limit),
};
#undef T

int main(int argc, char *argv[])
{
	char root[PATH_MAX];
	int has_memory_cma_acc, i;

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(tests));

	has_memory_cma_acc = proc_mount_contains("memory_cma_accounting");
	if (has_memory_cma_acc < 0)
		ksft_exit_skip("cannot query cgroup mount option.\n");
	else if (!has_memory_cma_acc)
		ksft_exit_skip("CMA accounting is disabled.\n");

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroupfs is not available.\n");

	if (cg_read_strstr(root, "cgroup.controllers", "memory"))
		ksft_exit_skip("memory controller is not available (CONFIG_CGROUP_MEMCG?)\n");

	for (i = 0; i < ARRAY_SIZE(tests); ++i) {
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

	ksft_finished();
}
