// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include "../kselftest.h"

#include <sys/vfs.h>
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif

#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0x4000000
#endif

static size_t ps;

struct test_case_t {
	const char *name;
	const char *pathname;
	int open_flags;
	int mmap_prot;
	int mmap_flags;
	int exp_err;
	int (*resolve_exp_err)(const char *path);
};

static int get_shm_expected_error(const char *path)
{
	struct statfs fs;

	if (statfs(path, &fs) == 0) {
		if (fs.f_type == RAMFS_MAGIC)
			return 0; /* ramfs succeed with contiguous memory */
	}
	 /* hostfs, etc returns ENODEV due to lack of contiguous allocation */
	return ENODEV;
}

static struct test_case_t test_cases[] = {
	{
		"Anonymous private allocation",
		NULL,
		O_CREAT | O_RDWR | O_EXCL,
		PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE,
		0,
		NULL,
	},
	{
		"Non-anonymous private file mapping (rw-)",
		"/tmp/ksft.nommu-reg-XXXXXX",
		O_CREAT | O_RDWR | O_EXCL,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE,
		0,
		0,
	},
	{
		"Non-anonymous private file mapping (r--)",
		"/tmp/ksft.nommu-reg-XXXXXX",
		O_CREAT | O_RDWR | O_EXCL,
		PROT_READ,
		MAP_PRIVATE,
		0,
		0,
	},
	{
		"Non-anonymous shared file mapping (rw-)",
		"/tmp/ksft.nommu-shm-XXXXXX",
		O_CREAT | O_RDWR | O_EXCL,
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		0,
#ifdef CONFIG_NOMMU
		get_shm_expected_error,
#else
		0,
#endif
	},
	{
		"Non-anonymous shared file mapping (r--)",
		"/tmp/ksft.nommu-shm-XXXXXX",
		O_CREAT | O_RDWR | O_EXCL,
		PROT_READ,
		MAP_SHARED,
		0,
#ifdef CONFIG_NOMMU
		get_shm_expected_error,
#else
		0,
#endif
	},
	{
		"Memory-backed private storage via /dev/zero",
		"/dev/zero",
		O_RDONLY,
		PROT_READ,
		MAP_PRIVATE,
		0,
		NULL,
	},
	{
		"Memory-backed storage via /dev/zero (MAP_SHARED)",
		"/dev/zero",
		O_RDONLY,
		PROT_READ,
		MAP_SHARED,
#ifdef CONFIG_NOMMU
		ENODEV,
#else
		0,
#endif
		NULL,
	},
	{
		"Block device volatile node mapping",
		"/dev/loop0",
		O_RDWR,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE,
		0,
		NULL,
	}
};

static int run_mapping_matrix_test(struct test_case_t *tcase)
{
	int fd;
	void *ptr;
	char path_buf[PATH_MAX];
	int rc = KSFT_PASS;
	int expected_error;

	ksft_print_msg("[RUN] Testing: %s\n", tcase->name);

	if (tcase->pathname == NULL) {
		fd = -1;
	} else if (strstr(tcase->pathname, "XXXXXX")) {
		strncpy(path_buf, tcase->pathname, sizeof(path_buf) - 1);
		path_buf[sizeof(path_buf) - 1] = '\0';
		fd = mkstemp(path_buf);
		if (fd < 0) {
			ksft_test_result_skip("Failed to setup temp node: %s\n",
					      tcase->pathname);
			return KSFT_SKIP;
		}
		if (ftruncate(fd, ps) != 0) {
			ksft_test_result_fail("ftruncate failed for: %s\n",
					      tcase->pathname);
			close(fd);
			unlink(path_buf);
			return KSFT_FAIL;
		}
	} else {
		fd = open(tcase->pathname, tcase->open_flags, 0600);
		if (fd < 0) {
			ksft_test_result_skip("Device node not accessible: %s\n",
				       tcase->pathname);
			return KSFT_SKIP;
		}
	}

	expected_error = tcase->exp_err;
	if (tcase->resolve_exp_err && fd >= 0)
		expected_error = tcase->resolve_exp_err(path_buf);

	ptr = mmap(NULL, ps, tcase->mmap_prot, tcase->mmap_flags, fd, 0);

	if (expected_error != 0) {
		if (ptr != MAP_FAILED) {
			ksft_test_result_fail("%s: mmap unexpectedly succeeded (exp error %d)\n",
					      tcase->name, expected_error);
			munmap(ptr, ps);
			rc = KSFT_FAIL;
			goto cleanup;
		}
		if (errno != expected_error) {
			ksft_test_result_fail("%s: mmap failed with %d (%s), but expected %d\n",
					      tcase->name, errno, strerror(errno), expected_error);
			rc = KSFT_FAIL;
			goto cleanup;
		}
		ksft_test_result_pass("%s: Correctly rejected with expected error %s(%d)\n",
				      tcase->name, strerror(expected_error), expected_error);
		rc = KSFT_PASS;
		goto cleanup;
	}

	if (ptr == MAP_FAILED) {
		ksft_test_result_fail("%s: mmap failed unexpectedly: %s\n",
				      tcase->name, strerror(errno));
		rc = KSFT_FAIL;
		goto cleanup;
	}

	ksft_test_result_pass("%s: mmap validation successfully passed\n", tcase->name);
	munmap(ptr, ps);

cleanup:
	if (fd >= 0) {
		close(fd);
		if (tcase->pathname && strstr(tcase->pathname, "XXXXXX"))
			unlink(path_buf);
	}
	return rc;
}

static int test_map_fixed(void)
{
	void *ptr = mmap((void *)(ps * 100), ps, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

	ksft_print_msg("[RUN] Testing MAP_FIXED behavior\n");

#ifdef CONFIG_NOMMU
	if (ptr == MAP_FAILED && (errno == ENODEV || errno == EINVAL)) {
		ksft_test_result_pass("MAP_FIXED correctly rejected under nommu\n");
		return KSFT_PASS;
	}
	if (ptr != MAP_FAILED) {
		ksft_test_result_fail("MAP_FIXED unexpectedly allowed under nommu\n");
		munmap(ptr, ps);
		return KSFT_FAIL;
	}
	ksft_test_result_fail("MAP_FIXED failed under NOMMU: %s\n",
			      strerror(errno));
	return KSFT_FAIL;
#else
	if (ptr != MAP_FAILED) {
		ksft_test_result_pass("MAP_FIXED successfully allocated under MMU\n");
		munmap(ptr, ps);
		return KSFT_PASS;
	}
	ksft_test_result_fail("MAP_FIXED failed allocation under MMU\n");
	return KSFT_FAIL;
#endif
}

static int test_uninit(void)
{
	void *ptr = mmap(NULL, ps, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_UNINITIALIZED, -1, 0);

	ksft_print_msg("[RUN] Testing MAP_UNINITIALIZED behavior\n");

	if (ptr == MAP_FAILED) {
		ksft_test_result_skip("MAP_UNINITIALIZED not supported by kernel config\n");
		return KSFT_SKIP;
	}

	ksft_test_result_pass("MAP_UNINITIALIZED allocation successful\n");
	munmap(ptr, ps);
	return KSFT_PASS;
}

int main(int argc, char **argv)
{
	int result = KSFT_PASS;
	int i, rc;

	ps = sysconf(_SC_PAGESIZE);
	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(test_cases) + 2);

#ifdef CONFIG_NOMMU
	ksft_print_msg("Running strict MMAP test criteria under nommu architecture\n");
#else
	ksft_print_msg("Running MMAP test criteria under MMU architecture\n");
#endif

	if (test_map_fixed() == KSFT_FAIL)
		result = KSFT_FAIL;

	if (test_uninit() == KSFT_FAIL)
		result = KSFT_FAIL;

	for (i = 0; i < (int)ARRAY_SIZE(test_cases); i++) {
		rc = run_mapping_matrix_test(&test_cases[i]);
		if (rc == KSFT_FAIL)
			result = KSFT_FAIL;
	}

	if (result == KSFT_PASS)
		ksft_finished();

	ksft_exit_fail();
}
