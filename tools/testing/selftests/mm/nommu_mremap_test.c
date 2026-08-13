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

static size_t ps;

static long get_fs_type(const char *path)
{
	struct statfs fs;

	if (statfs(path, &fs) == 0)
		return fs.f_type;

	return 0;
}

/* return original value if succeed */
static int set_nr_trim_pages(const char *value)
{
	int fd, orig_value;
	ssize_t len, written, read_len;
	char orig_buf[32];

	fd = open("/proc/sys/vm/nr_trim_pages", O_RDWR);
	if (fd < 0)
		return -errno;

	read_len = read(fd, orig_buf, sizeof(orig_buf) - 1);
	if (read_len < 0) {
		close(fd);
		return -errno;
	}
	if (read_len == 0) {
		close(fd);
		return -EIO;
	}

	orig_buf[read_len] = '\0';
	orig_value = atoi(orig_buf);

	if (lseek(fd, 0, SEEK_SET) < 0) {
		close(fd);
		return -errno;
	}

	len = strlen(value);
	written = write(fd, value, len);
	close(fd);

	if (written != len)
		return written < 0 ? -errno : -EIO;

	return orig_value >= 0 ? orig_value : -EINVAL;
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

static void zero_middle_munmap_test(void)
{
	int fd = -1;
	void *addr = MAP_FAILED;
	void *addr2 = MAP_FAILED;
	int left_mapped = 0;
	int right_mapped = 0;
	int ret;
	bool addr2_middle_unmapped = false;

	ksft_print_msg("[RUN] Testing middle munmap of private /dev/zero mappings\n");

	fd = open("/dev/zero", O_RDONLY);
	if (fd < 0) {
		ksft_test_result_skip("Unable to open /dev/zero: %s\n",
				      strerror(errno));
		return;
	}

	addr = mmap(NULL, ps * 3, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE, fd, 0);
	close(fd);
	fd = -1;
	if (addr == MAP_FAILED) {
		ksft_test_result_fail("Initial /dev/zero mmap failed: %s\n",
				      strerror(errno));
		return;
	}

	left_mapped = 1;
	right_mapped = 1;

	/*
	 * Split one private /dev/zero VMA in the middle. On nommu, the
	 * VMA still has vm_file set even though it is semantically
	 * anonymous.
	 */
	ret = munmap((char *)addr + ps, ps);
	if (ret) {
		ksft_test_result_fail("Middle munmap failed: %s\n",
				      strerror(errno));
		munmap(addr, ps * 3);
		left_mapped = 0;
		right_mapped = 0;
		goto out;
	}

	/*
	 * Access both remaining VMAs so a stale VMA interval cannot remain
	 * hidden behind the unmapped hole.
	 */
	((volatile unsigned char *)addr)[0] = 0x5a;
	((volatile unsigned char *)addr + 2 * ps)[0] = 0xa5;

	/*
	 * Repeat the operation on another mapping of the same inode. This
	 * exercises insertion and removal in /dev/zero's i_mmap interval
	 * tree after the first split.
	 */
	fd = open("/dev/zero", O_RDONLY);
	if (fd < 0) {
		ksft_test_result_fail("Reopening /dev/zero failed: %s\n",
				      strerror(errno));
		goto out;
	}

	addr2 = mmap(NULL, ps * 3, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE, fd, 0);
	close(fd);
	fd = -1;
	if (addr2 == MAP_FAILED) {
		ksft_test_result_fail("Second /dev/zero mmap failed: %s\n",
				      strerror(errno));
		goto out;
	}

	ret = munmap((char *)addr2 + ps, ps);
	if (ret) {
		ksft_test_result_fail("Second middle munmap failed: %s\n",
				      strerror(errno));
		goto out;
	}
	addr2_middle_unmapped = true;

	if (munmap(addr, ps)) {
		ksft_test_result_fail("Left VMA munmap failed: %s\n",
				      strerror(errno));
		goto out;
	}
	left_mapped = 0;

	if (munmap((char *)addr + 2 * ps, ps)) {
		ksft_test_result_fail("Right VMA munmap failed: %s\n",
				      strerror(errno));
		goto out;
	}
	right_mapped = 0;

	if (munmap(addr2, ps)) {
		ksft_test_result_fail("Second left VMA munmap failed: %s\n",
				      strerror(errno));
		goto out;
	}

	if (munmap((char *)addr2 + 2 * ps, ps)) {
		ksft_test_result_fail("Second right VMA munmap failed: %s\n",
				      strerror(errno));
		goto out;
	}
	addr2 = MAP_FAILED;

	ksft_test_result_pass("%s success\n", __func__);

out:
	if (fd >= 0)
		close(fd);
	if (left_mapped)
		munmap(addr, ps);
	if (right_mapped)
		munmap((char *)addr + 2 * ps, ps);
	if (addr2 != MAP_FAILED) {
		if (addr2_middle_unmapped) {
			munmap(addr2, ps);
			munmap((char *)addr2 + 2 * ps, ps);
		} else {
			/* The middle munmap failed, so all three pages remain mapped. */
			munmap(addr2, ps * 3);
		}
	}
}

static int get_shared_writable_file_expected_error(const char *path)
{
	if (get_fs_type(path) == RAMFS_MAGIC)
		return EPERM; /* ramfs failed */

	return 0;
}

static int pre_conf_trim_page(void)
{
	return set_nr_trim_pages("0\n");
}

static int post_conf_trim_page(int value)
{
	char buf[32];

	if (snprintf(buf, sizeof(buf), "%d\n", value) < 0)
		return -EIO;

	return set_nr_trim_pages(buf);
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
	int (*pre_hook)(void);
	int (*post_hook)(int value);
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
		.name = "zero device shrink (r--)",
		.pathname = "/dev/zero",
		.open_flags = O_RDONLY,
		.mmap_prot = PROT_READ,
		.mmap_flags = MAP_PRIVATE,
		.exp_err = 0,
		.resolve_exp_err = 0,
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
		.name = "private file growth with reserved capacity (rw-)",
		.pathname = "/tmp/ksft.nommu-remap-XXXXXX",
		.open_flags = O_CREAT | O_RDWR | O_EXCL,
		.mmap_prot = PROT_READ | PROT_WRITE,
		.mmap_flags = MAP_PRIVATE,
		.exp_err = 0,
		.resolve_exp_err = 0,
		.old_pages = 3,
		.new_pages = 4,
#ifdef CONFIG_NOMMU
		.pre_hook = pre_conf_trim_page,
		.post_hook = post_conf_trim_page,
#endif
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
	int orig_nr_trip_pages = -1;

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

	if (tcase->pre_hook) {
		orig_nr_trip_pages = tcase->pre_hook();
		if (orig_nr_trip_pages < 0) {
			ksft_print_msg("pre-hook failed %s(%d)\n",
					    strerror(errno), errno);
			rc = KSFT_FAIL;
			goto out;
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

	if (tcase->post_hook) {
		if (tcase->post_hook(orig_nr_trip_pages) < 0) {
			ksft_print_msg("post-hook failed %s(%d)\n",
				       strerror(errno), errno);
			rc = KSFT_FAIL;
		}
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
	ksft_set_plan(ARRAY_SIZE(mremap_cases) + 3);

	munmap_shrink_test();
	mremap_shrink_test();
	zero_middle_munmap_test();

	for (i = 0; i < (int)ARRAY_SIZE(mremap_cases); i++) {
		if (run_mremap_test(&mremap_cases[i]) == KSFT_FAIL)
			res = KSFT_FAIL;
	}

	if (res == KSFT_PASS)
		ksft_finished();

	ksft_exit_fail();
}
