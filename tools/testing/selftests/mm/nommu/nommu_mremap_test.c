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
#include "kselftest.h"

#include <sys/vfs.h>
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif

static size_t ps;

static long get_fs_type(const char *path)
{
	struct statfs fs;

	if (statfs(path, &fs) == 0)
		return fs.f_type;

	return 0;
}

static void munmap_shrink_test(void)
{
	void *addr;

	/* munmap shrink test */
	for (int i = 0; i < 4; i++) {
		addr = mmap(NULL, ps * 4, PROT_READ | PROT_WRITE,
			    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (addr == MAP_FAILED) {
			ksft_test_result_fail("mmap failed: %s(%d)\n", strerror(errno), errno);
			return;
		}
		if (munmap(addr + ps * i, ps) != 0) {
			ksft_test_result_fail("memory %p isn't unmapped at %p\n",
					      addr, addr + ps * i);
			for (int j = 0; j < 4; j++)
				munmap((char *)addr + j * ps, ps);
			return;
		}

		if (i == 0) {
			if (munmap(addr +  ps, ps * 3))
				goto error;
		} else if (i == 1) {
			if (munmap(addr, ps) || munmap(addr + (ps * 2), ps * 2))
				goto error;
		} else if (i == 2) {
			if (munmap(addr, ps * 2) || munmap(addr + (ps * 3), ps))
				goto error;
		} else if (i == 3) {
			if (munmap(addr, ps * 3))
				goto error;
		}
	}

	ksft_test_result_pass("%s success\n", __func__);
	return;
error:
	for (int j = 0; j < 4; j++)
		munmap((char *)addr + j * ps, ps);
	ksft_test_result_fail("%s clean up failures\n", __func__);
}

static size_t page_align(size_t len)
{
	return (len + ps - 1) / ps * ps;
}

static void mremap_shrink_test(void)
{
	void *addr, *addr2;
	size_t current_len;
	size_t old_len, new_len;
	struct param {
		size_t old;
		size_t new;
	} params[] = {
		/* should not happen any shrink */
		{ .old = ps * 4 - 1, .new = ps * 4 - 2 },
		/* should not happen any shrink */
		{ .old = ps * 4 - 1, .new = ps * 4 },
		{ .old = ps * 4, .new = ps * 2 },
		/* should not happen any shrink */
		{ .old = ps * 2, .new = ps * 2 - 2 },
		{ .old = ps * 2 - 2, .new = ps * 1 },
	};

	/* mremap shrink test */
	current_len = page_align(ps * 4 - 1);
	addr = mmap(NULL, ps * 4 - 1, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (addr == MAP_FAILED) {
		ksft_test_result_fail("mmap failed: %s(%d)\n", strerror(errno), errno);
		return;
	}

	for (int i = 0; i < ARRAY_SIZE(params); i++) {
		old_len = page_align(params[i].old);
		new_len = page_align(params[i].new);
		addr2 = mremap(addr, old_len, new_len, MREMAP_MAYMOVE);
		if (addr2 == MAP_FAILED) {
			ksft_test_result_fail("memory %p isn't remapped at %p\n", addr, addr2);
			munmap(addr, old_len);
			return;
		}

		addr = addr2;
		current_len = new_len;
	}

	if (munmap(addr, current_len)) {
		ksft_test_result_fail("%s cleanup failed: %s\n",
				      __func__, strerror(errno));
		return;
	}
	ksft_test_result_pass("%s success\n", __func__);
}

static int get_shared_writable_file_expected_error(const char *path)
{
	if (get_fs_type(path) == RAMFS_MAGIC)
		return EPERM; /* ramfs failed */

	return 0;
}

struct mremap_case_t {
	const char *name;
	const char *pathname;
	int open_flags;
	int mmap_prot;
	int mmap_flags;
	int exp_err;
	int (*resolve_exp_err)(const char *path);
	unsigned int old_pages;
	unsigned int new_pages;
};

static struct mremap_case_t mremap_cases[] = {
	{
		.name = "anonymous shrink (r--)",
		.pathname = NULL,
		.open_flags = O_CREAT | O_RDWR | O_EXCL,
		.mmap_prot = PROT_READ,
		.mmap_flags = MAP_ANONYMOUS | MAP_PRIVATE,
		.exp_err = 0,
		.resolve_exp_err = 0,
	},
	{
		.name = "shared file shrink (r--)",
		.pathname = "/tmp/ksft.nommu-remap-XXXXXX",
		.open_flags = O_CREAT | O_RDWR | O_EXCL,
		.mmap_prot = PROT_READ,
		.mmap_flags = MAP_SHARED,
		.exp_err = 0,
#ifdef CONFIG_NOMMU
		.resolve_exp_err = get_shared_writable_file_expected_error,
#else
		.resolve_exp_err = 0,
#endif
	},
	{
		.name = "private file unchanged length (r-)",
		.pathname = "/tmp/ksft.nommu-remap-XXXXXX",
		.open_flags = O_CREAT | O_RDWR | O_EXCL,
		.mmap_prot = PROT_READ,
		.mmap_flags = MAP_PRIVATE,
#ifdef CONFIG_NOMMU
		.exp_err = EPERM,
#else
		.exp_err = 0,
#endif
		.resolve_exp_err = 0,
		.old_pages = 4,
		.new_pages = 4,
	},
	{
		.name = "private file unchanged length (rw-)",
		.pathname = "/tmp/ksft.nommu-remap-XXXXXX",
		.open_flags = O_CREAT | O_RDWR | O_EXCL,
		.mmap_prot = PROT_READ | PROT_WRITE,
		.mmap_flags = MAP_PRIVATE,
		.exp_err = 0,
		.resolve_exp_err = 0,
		.old_pages = 4,
		.new_pages = 4,
	},
	{
		.name = "private file growth (r-)",
		.pathname = "/tmp/ksft.nommu-remap-XXXXXX",
		.open_flags = O_CREAT | O_RDWR | O_EXCL,
		.mmap_prot = PROT_READ,
		.mmap_flags = MAP_PRIVATE,
#ifdef CONFIG_NOMMU
		.exp_err = EPERM,
#else
		.exp_err = 0,
#endif
		.resolve_exp_err = 0,
		.old_pages = 4,
		.new_pages = 8,
	},
	{
		.name = "private file growth (rw-)",
		.pathname = "/tmp/ksft.nommu-remap-XXXXXX",
		.open_flags = O_CREAT | O_RDWR | O_EXCL,
		.mmap_prot = PROT_READ | PROT_WRITE,
		.mmap_flags = MAP_PRIVATE,
#ifdef CONFIG_NOMMU
		.exp_err = ENOMEM,
#else
		.exp_err = 0,
#endif
		.resolve_exp_err = 0,
		.old_pages = 4,
		.new_pages = 8,
	},
};

static int run_mremap_test(struct mremap_case_t *tcase)
{
	int fd = -1;
	void *addr, *addr2;
	char pb[PATH_MAX];
	int rc = KSFT_PASS;
	int expected_error;
	unsigned int old_pages = tcase->old_pages ?: 4;
	unsigned int new_pages = tcase->new_pages ?: 2;
	unsigned int file_pages = old_pages > new_pages ?
				  old_pages : new_pages;
	int orig_nr_trim_pages = -1;

	ksft_print_msg("[RUN] Testing mremap: %s\n", tcase->name);

	if (tcase->pathname && strstr(tcase->pathname, "XXXXXX")) {
		strncpy(pb, tcase->pathname, sizeof(pb) - 1);
		pb[sizeof(pb) - 1] = '\0';
		fd = mkstemp(pb);
		if (fd < 0) {
			ksft_test_result_skip("Failed to setup file backing\n");
			return KSFT_SKIP;
		}
		if (ftruncate(fd, ps * file_pages) != 0) {
			ksft_test_result_fail("Failed to setup file backing\n");
			close(fd);
			unlink(pb);
			return KSFT_FAIL;
		}

		if ((tcase->mmap_flags & MAP_SHARED) && get_fs_type(pb) != RAMFS_MAGIC) {
			ksft_test_result_skip("Skip the test under non-ramfs filesystem (%s)\n",
					      pb);
			close(fd);
			unlink(pb);
			return KSFT_SKIP;
		}
	} else if (tcase->pathname) {
		fd = open(tcase->pathname, tcase->open_flags, 0600);
		if (fd < 0) {
			ksft_test_result_skip("Backing node not accessible\n");
			return KSFT_SKIP;
		}

		if ((tcase->mmap_flags & MAP_SHARED) &&
		    get_fs_type(tcase->pathname) != RAMFS_MAGIC) {
			ksft_test_result_skip("Skip the test under non-ramfs filesystem (%s)\n",
					      tcase->pathname);
			close(fd);
			return KSFT_SKIP;
		}
	}

	addr = mmap(NULL, ps * old_pages, tcase->mmap_prot,
		    tcase->mmap_flags, fd, 0);
	if (addr == MAP_FAILED) {
		ksft_print_msg("mmap mapping failed %s(%d)\n", strerror(errno), errno);
		rc = KSFT_FAIL;
		goto out;
	}

	expected_error = tcase->exp_err;
	if (tcase->resolve_exp_err && fd >= 0)
		expected_error = tcase->resolve_exp_err(pb);

	addr2 = mremap(addr, ps * old_pages, ps * new_pages,
		       MREMAP_MAYMOVE);

	if (expected_error != 0) {
		if (addr2 != MAP_FAILED || errno != expected_error) {
			ksft_print_msg("Expected error %d, got %s(%d)\n",
				       expected_error, strerror(errno), errno);
			rc = KSFT_FAIL;
		} else {
			ksft_print_msg("%s/%s: Handled expected error path (errno=%d)\n",
				       __func__, tcase->name, expected_error);
		}
	} else if (addr2 == MAP_FAILED) {
		ksft_print_msg("mremap shrink failed unexpectedly: %s\n",
			       strerror(errno));
		rc = KSFT_FAIL;
	} else {
		ksft_print_msg("%s/%s step successful\n", __func__, tcase->name);
	}

	/* clean up */
	if (munmap(addr2 == MAP_FAILED ? addr : addr2,
		   addr2 == MAP_FAILED ? ps * old_pages : ps * new_pages)) {
		ksft_print_msg("munmap failed: %s\n", strerror(errno));
		rc = KSFT_FAIL;
	}

out:
	if (fd >= 0) {
		close(fd);
		if (tcase->pathname && strstr(tcase->pathname, "XXXXXX"))
			unlink(pb);
	}

	ksft_test_result_report(rc, "%s:%s\n", __func__, tcase->name);
	return rc;
}

int main(int argc, char **argv)
{
	int res = KSFT_PASS;
	int i;

	ps = sysconf(_SC_PAGESIZE);
	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(mremap_cases) + 2);

	munmap_shrink_test();
	mremap_shrink_test();

	for (i = 0; i < (int)ARRAY_SIZE(mremap_cases); i++) {
		if (run_mremap_test(&mremap_cases[i]) == KSFT_FAIL)
			res = KSFT_FAIL;
	}

	if (res == KSFT_PASS)
		ksft_finished();

	ksft_exit_fail();
}
