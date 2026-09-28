// SPDX-License-Identifier: GPL-2.0

/*
 * Tests for mremap w/ MREMAP_DONTUNMAP.
 *
 * Copyright 2020, Brian Geffon <bgeffon@google.com>
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/mman.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kselftest.h"

unsigned long page_size;
char *page_buffer;

static void dump_maps(void)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "cat /proc/%d/maps", getpid());
	system(cmd);
}

#define BUG_ON(condition, description)						\
	do {									\
		if (condition) {						\
			dump_maps();						\
			ksft_exit_fail_msg("[FAIL]\t%s:%d\t%s:%s\n",		\
					   __func__, __LINE__, (description),	\
					   strerror(errno));			\
		}								\
	} while (0)

/*
 * Same as mlock2.h's, plus an ENOSYS fallback for libc headers without
 * __NR_mlock2.  It is not taken from the header because that also defines
 * seek_to_smaps_entry(), which nothing here uses and which then warns
 * (-Wunused-function).
 */
static int mlock2_(void *start, size_t len, int flags)
{
#ifdef __NR_mlock2
	return syscall(__NR_mlock2, start, len, flags);
#else
	errno = ENOSYS;
	return -1;
#endif
}

/*
 * Locked memory size in kB, as reported by /proc/self/status, which is
 * mm->locked_vm accounted in kB.  Used to check that the mlock() accounting
 * balances across a MREMAP_DONTUNMAP operation.
 *
 * Returns LOCKED_VM_UNKNOWN if it cannot be read: the callers run in a child
 * whose exit status is the test result, so this must not exit the process or
 * print anything the TAP output parser would act on.
 */
#define LOCKED_VM_UNKNOWN	((unsigned long)-1)

static unsigned long get_proc_locked_vm_size(void)
{
	unsigned long lock_size;
	char *line = NULL;
	size_t size = 0;
	FILE *f;

	f = fopen("/proc/self/status", "r");
	if (!f) {
		fprintf(stderr, "cannot open /proc/self/status: %s\n",
			strerror(errno));
		return LOCKED_VM_UNKNOWN;
	}

	while (getline(&line, &size, f) != -1) {
		if (sscanf(line, "VmLck:\t%8lu kB", &lock_size) == 1) {
			free(line);
			fclose(f);
			return lock_size;
		}
	}

	free(line);
	fclose(f);
	fprintf(stderr, "cannot parse VmLck in /proc/self/status\n");
	return LOCKED_VM_UNKNOWN;
}

// Try a simple operation for to "test" for kernel support this prevents
// reporting tests as failed when it's run on an older kernel.
static int kernel_support_for_mremap_dontunmap()
{
	int ret = 0;
	unsigned long num_pages = 1;
	void *source_mapping = mmap(NULL, num_pages * page_size, PROT_NONE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(source_mapping == MAP_FAILED, "mmap");

	// This simple remap should only fail if MREMAP_DONTUNMAP isn't
	// supported.
	void *dest_mapping =
	    mremap(source_mapping, num_pages * page_size, num_pages * page_size,
		   MREMAP_DONTUNMAP | MREMAP_MAYMOVE, 0);
	if (dest_mapping == MAP_FAILED) {
		ret = errno;
	} else {
		BUG_ON(munmap(dest_mapping, num_pages * page_size) == -1,
		       "unable to unmap destination mapping");
	}

	BUG_ON(munmap(source_mapping, num_pages * page_size) == -1,
	       "unable to unmap source mapping");
	return ret;
}

// This helper will just validate that an entire mapping contains the expected
// byte.
static int check_region_contains_byte(void *addr, unsigned long size, char byte)
{
	BUG_ON(size & (page_size - 1),
	       "check_region_contains_byte expects page multiples");
	BUG_ON((unsigned long)addr & (page_size - 1),
	       "check_region_contains_byte expects page alignment");

	memset(page_buffer, byte, page_size);

	unsigned long num_pages = size / page_size;
	unsigned long i;

	// Compare each page checking that it contains our expected byte.
	for (i = 0; i < num_pages; ++i) {
		int ret =
		    memcmp(addr + (i * page_size), page_buffer, page_size);
		if (ret) {
			return ret;
		}
	}

	return 0;
}

// this test validates that MREMAP_DONTUNMAP moves the pagetables while leaving
// the source mapping mapped.
static void mremap_dontunmap_simple()
{
	unsigned long num_pages = 5;

	void *source_mapping =
	    mmap(NULL, num_pages * page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(source_mapping == MAP_FAILED, "mmap");

	memset(source_mapping, 'a', num_pages * page_size);

	// Try to just move the whole mapping anywhere (not fixed).
	void *dest_mapping =
	    mremap(source_mapping, num_pages * page_size, num_pages * page_size,
		   MREMAP_DONTUNMAP | MREMAP_MAYMOVE, NULL);
	BUG_ON(dest_mapping == MAP_FAILED, "mremap");

	// Validate that the pages have been moved, we know they were moved if
	// the dest_mapping contains a's.
	BUG_ON(check_region_contains_byte
	       (dest_mapping, num_pages * page_size, 'a') != 0,
	       "pages did not migrate");
	BUG_ON(check_region_contains_byte
	       (source_mapping, num_pages * page_size, 0) != 0,
	       "source should have no ptes");

	BUG_ON(munmap(dest_mapping, num_pages * page_size) == -1,
	       "unable to unmap destination mapping");
	BUG_ON(munmap(source_mapping, num_pages * page_size) == -1,
	       "unable to unmap source mapping");
	ksft_test_result_pass("%s\n", __func__);
}

// This test validates that MREMAP_DONTUNMAP on a shared mapping works as expected.
static void mremap_dontunmap_simple_shmem()
{
	unsigned long num_pages = 5;

	int mem_fd = memfd_create("memfd", MFD_CLOEXEC);
	BUG_ON(mem_fd < 0, "memfd_create");

	BUG_ON(ftruncate(mem_fd, num_pages * page_size) < 0,
			"ftruncate");

	void *source_mapping =
	    mmap(NULL, num_pages * page_size, PROT_READ | PROT_WRITE,
		 MAP_FILE | MAP_SHARED, mem_fd, 0);
	BUG_ON(source_mapping == MAP_FAILED, "mmap");

	BUG_ON(close(mem_fd) < 0, "close");

	memset(source_mapping, 'a', num_pages * page_size);

	// Try to just move the whole mapping anywhere (not fixed).
	void *dest_mapping =
	    mremap(source_mapping, num_pages * page_size, num_pages * page_size,
		   MREMAP_DONTUNMAP | MREMAP_MAYMOVE, NULL);
	if (dest_mapping == MAP_FAILED && errno == EINVAL) {
		// Old kernel which doesn't support MREMAP_DONTUNMAP on shmem.
		BUG_ON(munmap(source_mapping, num_pages * page_size) == -1,
			"unable to unmap source mapping");
		return;
	}

	BUG_ON(dest_mapping == MAP_FAILED, "mremap");

	// Validate that the pages have been moved, we know they were moved if
	// the dest_mapping contains a's.
	BUG_ON(check_region_contains_byte
	       (dest_mapping, num_pages * page_size, 'a') != 0,
	       "pages did not migrate");

	// Because the region is backed by shmem, we will actually see the same
	// memory at the source location still.
	BUG_ON(check_region_contains_byte
	       (source_mapping, num_pages * page_size, 'a') != 0,
	       "source should have no ptes");

	BUG_ON(munmap(dest_mapping, num_pages * page_size) == -1,
	       "unable to unmap destination mapping");
	BUG_ON(munmap(source_mapping, num_pages * page_size) == -1,
	       "unable to unmap source mapping");
	ksft_test_result_pass("%s\n", __func__);
}

// This test validates MREMAP_DONTUNMAP will move page tables to a specific
// destination using MREMAP_FIXED, also while validating that the source
// remains intact.
static void mremap_dontunmap_simple_fixed()
{
	unsigned long num_pages = 5;

	// Since we want to guarantee that we can remap to a point, we will
	// create a mapping up front.
	void *dest_mapping =
	    mmap(NULL, num_pages * page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(dest_mapping == MAP_FAILED, "mmap");
	memset(dest_mapping, 'X', num_pages * page_size);

	void *source_mapping =
	    mmap(NULL, num_pages * page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(source_mapping == MAP_FAILED, "mmap");
	memset(source_mapping, 'a', num_pages * page_size);

	void *remapped_mapping =
	    mremap(source_mapping, num_pages * page_size, num_pages * page_size,
		   MREMAP_FIXED | MREMAP_DONTUNMAP | MREMAP_MAYMOVE,
		   dest_mapping);
	BUG_ON(remapped_mapping == MAP_FAILED, "mremap");
	BUG_ON(remapped_mapping != dest_mapping,
	       "mremap should have placed the remapped mapping at dest_mapping");

	// The dest mapping will have been unmap by mremap so we expect the Xs
	// to be gone and replaced with a's.
	BUG_ON(check_region_contains_byte
	       (dest_mapping, num_pages * page_size, 'a') != 0,
	       "pages did not migrate");

	// And the source mapping will have had its ptes dropped.
	BUG_ON(check_region_contains_byte
	       (source_mapping, num_pages * page_size, 0) != 0,
	       "source should have no ptes");

	BUG_ON(munmap(dest_mapping, num_pages * page_size) == -1,
	       "unable to unmap destination mapping");
	BUG_ON(munmap(source_mapping, num_pages * page_size) == -1,
	       "unable to unmap source mapping");
	ksft_test_result_pass("%s\n", __func__);
}

// This test validates that we can MREMAP_DONTUNMAP for a portion of an
// existing mapping.
static void mremap_dontunmap_partial_mapping()
{
	/*
	 *  source mapping:
	 *  --------------
	 *  | aaaaaaaaaa |
	 *  --------------
	 *  to become:
	 *  --------------
	 *  | aaaaa00000 |
	 *  --------------
	 *  With the destination mapping containing 5 pages of As.
	 *  ---------
	 *  | aaaaa |
	 *  ---------
	 */
	unsigned long num_pages = 10;
	void *source_mapping =
	    mmap(NULL, num_pages * page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(source_mapping == MAP_FAILED, "mmap");
	memset(source_mapping, 'a', num_pages * page_size);

	// We will grab the last 5 pages of the source and move them.
	void *dest_mapping =
	    mremap(source_mapping + (5 * page_size), 5 * page_size,
		   5 * page_size,
		   MREMAP_DONTUNMAP | MREMAP_MAYMOVE, NULL);
	BUG_ON(dest_mapping == MAP_FAILED, "mremap");

	// We expect the first 5 pages of the source to contain a's and the
	// final 5 pages to contain zeros.
	BUG_ON(check_region_contains_byte(source_mapping, 5 * page_size, 'a') !=
	       0, "first 5 pages of source should have original pages");
	BUG_ON(check_region_contains_byte
	       (source_mapping + (5 * page_size), 5 * page_size, 0) != 0,
	       "final 5 pages of source should have no ptes");

	// Finally we expect the destination to have 5 pages worth of a's.
	BUG_ON(check_region_contains_byte(dest_mapping, 5 * page_size, 'a') !=
	       0, "dest mapping should contain ptes from the source");

	BUG_ON(munmap(dest_mapping, 5 * page_size) == -1,
	       "unable to unmap destination mapping");
	BUG_ON(munmap(source_mapping, num_pages * page_size) == -1,
	       "unable to unmap source mapping");
	ksft_test_result_pass("%s\n", __func__);
}

// This test validates that we can remap over only a portion of a mapping.
static void mremap_dontunmap_partial_mapping_overwrite(void)
{
	/*
	 *  source mapping:
	 *  ---------
	 *  |aaaaa|
	 *  ---------
	 *  dest mapping initially:
	 *  -----------
	 *  |XXXXXXXXXX|
	 *  ------------
	 *  Source to become:
	 *  ---------
	 *  |00000|
	 *  ---------
	 *  With the destination mapping containing 5 pages of As.
	 *  ------------
	 *  |aaaaaXXXXX|
	 *  ------------
	 */
	void *source_mapping =
	    mmap(NULL, 5 * page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(source_mapping == MAP_FAILED, "mmap");
	memset(source_mapping, 'a', 5 * page_size);

	void *dest_mapping =
	    mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(dest_mapping == MAP_FAILED, "mmap");
	memset(dest_mapping, 'X', 10 * page_size);

	// We will grab the last 5 pages of the source and move them.
	void *remapped_mapping =
	    mremap(source_mapping, 5 * page_size,
		   5 * page_size,
		   MREMAP_DONTUNMAP | MREMAP_MAYMOVE | MREMAP_FIXED, dest_mapping);
	BUG_ON(remapped_mapping == MAP_FAILED, "mremap");
	BUG_ON(dest_mapping != remapped_mapping, "expected to remap to dest_mapping");

	BUG_ON(check_region_contains_byte(source_mapping, 5 * page_size, 0) !=
	       0, "first 5 pages of source should have no ptes");

	// Finally we expect the destination to have 5 pages worth of a's.
	BUG_ON(check_region_contains_byte(dest_mapping, 5 * page_size, 'a') != 0,
			"dest mapping should contain ptes from the source");

	// Finally the last 5 pages shouldn't have been touched.
	BUG_ON(check_region_contains_byte(dest_mapping + (5 * page_size),
				5 * page_size, 'X') != 0,
			"dest mapping should have retained the last 5 pages");

	BUG_ON(munmap(dest_mapping, 10 * page_size) == -1,
	       "unable to unmap destination mapping");
	BUG_ON(munmap(source_mapping, 5 * page_size) == -1,
	       "unable to unmap source mapping");
	ksft_test_result_pass("%s\n", __func__);
}

/*
 * Child exit codes for the accounting cases: any other exit code, and any
 * signal, is reported as an error rather than mistaken for a leak.
 */
#define CASE_LEAK		2
#define CASE_SKIP		77
#define CASE_SKIP_ENOSYS	78
#define CASE_SETUP_ERROR	79

/* Report a setup failure from a case: the child's exit status carries it back. */
static int case_failed(const char *where, const char *what)
{
	fprintf(stderr, "%s: %s: %s\n", where, what, strerror(errno));
	return CASE_SETUP_ERROR;
}

/* Report a check which failed for a reason errno does not describe. */
static int case_unexpected(const char *where, const char *what)
{
	fprintf(stderr, "%s: unexpected %s\n", where, what);
	return CASE_SETUP_ERROR;
}

/*
 * Only EPERM/ENOMEM are expected with a small RLIMIT_MEMLOCK, and ENOSYS means
 * the kernel has no mlock2(); anything else is a genuine setup failure.
 */
static int lock_failed(const char *where, const char *call)
{
	if (errno == EPERM || errno == ENOMEM)
		return CASE_SKIP;
	if (errno == ENOSYS)
		return CASE_SKIP_ENOSYS;

	return case_failed(where, call);
}

/*
 * Run one accounting case in a child, so that it starts with a clean mm and an
 * empty VmLck, and report its outcome.
 */
static void run_locked_case(const char *label, int (*fn)(void))
{
	int status;
	pid_t pid;

	/* do not let the child flush a copy of our TAP output */
	fflush(NULL);

	pid = fork();
	if (pid < 0) {
		ksft_test_result_error("%s: fork: %s\n", label,
				       strerror(errno));
		return;
	}
	if (!pid)
		_exit(fn());

	if (waitpid(pid, &status, 0) == -1) {
		ksft_test_result_error("%s: waitpid: %s\n", label,
				       strerror(errno));
		return;
	}

	if (WIFSIGNALED(status)) {
		ksft_test_result_error("%s: killed by signal %d\n", label,
				       WTERMSIG(status));
		return;
	}

	if (!WIFEXITED(status)) {
		ksft_test_result_error("%s: child did not exit\n", label);
		return;
	}

	switch (WEXITSTATUS(status)) {
	case 0:
		ksft_test_result_pass("%s: locked memory released\n", label);
		break;
	case CASE_LEAK:
		ksft_test_result_fail("%s: locked memory leaked\n", label);
		break;
	case CASE_SKIP:
		ksft_test_result_skip("%s: mlock not permitted\n", label);
		break;
	case CASE_SKIP_ENOSYS:
		ksft_test_result_skip("%s: mlock2 not supported\n", label);
		break;
	default:
		ksft_test_result_error("%s: child exited with %d (see stderr)\n",
				       label, WEXITSTATUS(status));
		break;
	}
}

/*
 * An unfaulted mlock-on-fault VMA moved behind itself: the source and
 * destination VMAs are adjacent and mergeable, and merging them clears the
 * mlock flags of the single resulting VMA, leaking mm->locked_vm.
 */
static int case_mlock_onfault_self_merge(void)
{
	unsigned long locked;
	void *source, *dest, *reserve;

	/*
	 * Two adjacent pages: the source VMA goes in the first, the
	 * destination in the second, so the two are mergeable.
	 */
	reserve = mmap(NULL, 2 * page_size, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reserve == MAP_FAILED)
		return case_failed(__func__, "mmap reserve");
	if (munmap(reserve, 2 * page_size) == -1)
		return case_failed(__func__, "munmap reserve");

	source = mmap(reserve, page_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (source != reserve)
		return case_unexpected(__func__, "source address");

	/* Locked on fault, but deliberately left unfaulted. */
	if (mlock2_(source, page_size, MLOCK_ONFAULT))
		return lock_failed(__func__, "mlock2");

	dest = mremap(source, page_size, page_size,
		      MREMAP_DONTUNMAP | MREMAP_MAYMOVE | MREMAP_FIXED,
		      source + page_size);
	if (dest == MAP_FAILED)
		return case_failed(__func__, "mremap");

	if (munmap(dest, page_size) == -1)
		return case_failed(__func__, "munmap destination");
	if (munmap(source, page_size) == -1)
		return case_failed(__func__, "munmap source");

	locked = get_proc_locked_vm_size();
	if (locked == LOCKED_VM_UNKNOWN)
		return CASE_SETUP_ERROR;

	return locked ? CASE_LEAK : 0;
}

/*
 * A partial MREMAP_DONTUNMAP of a locked VMA: all but the last page is moved,
 * leaving the source VMA mapped.  Both the moved pages and the VMA left
 * behind must give up their mlock accounting.
 *
 * The destination goes into a window with a guard page on either side, so that
 * it is not adjacent to and cannot merge with the source VMA: this case must
 * exercise the partial-copy accounting on its own.  The window's hole is
 * smaller than the source mapping, so the source cannot land in it.
 */
static int case_locked_partial(int onfault)
{
	unsigned long num_pages = 3;
	unsigned long span = (num_pages - 1) * page_size;
	unsigned long locked;
	void *source, *guard, *dest, *moved;

	guard = mmap(NULL, span + 2 * page_size, PROT_NONE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (guard == MAP_FAILED)
		return case_failed(__func__, "mmap guard");
	dest = guard + page_size;
	if (munmap(dest, span) == -1)
		return case_failed(__func__, "munmap destination window");

	source = mmap(NULL, num_pages * page_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (source == MAP_FAILED)
		return case_failed(__func__, "mmap");

	if (onfault) {
		if (mlock2_(source, num_pages * page_size, MLOCK_ONFAULT))
			return lock_failed(__func__, "mlock2");
	} else if (mlock(source, num_pages * page_size)) {
		return lock_failed(__func__, "mlock");
	}

	/* Move all but the last page, leaving the source partially mapped. */
	moved = mremap(source, span, span,
		       MREMAP_DONTUNMAP | MREMAP_MAYMOVE | MREMAP_FIXED, dest);
	if (moved == MAP_FAILED)
		return case_failed(__func__, "mremap");
	if (moved != dest)
		return case_unexpected(__func__, "destination address");

	if (munmap(dest, span) == -1)
		return case_failed(__func__, "munmap destination");
	if (munmap(source, num_pages * page_size) == -1)
		return case_failed(__func__, "munmap source");

	locked = get_proc_locked_vm_size();
	if (locked == LOCKED_VM_UNKNOWN)
		return CASE_SETUP_ERROR;

	return locked ? CASE_LEAK : 0;
}

static int case_locked_partial_mlock(void)
{
	return case_locked_partial(0);
}

static int case_locked_partial_onfault(void)
{
	return case_locked_partial(1);
}

int main(void)
{
	ksft_print_header();

	page_size = sysconf(_SC_PAGE_SIZE);

	// test for kernel support for MREMAP_DONTUNMAP skipping the test if
	// not.
	if (kernel_support_for_mremap_dontunmap() != 0) {
		ksft_print_msg("No kernel support for MREMAP_DONTUNMAP\n");
		ksft_finished();
	}

	ksft_set_plan(8);

	// Keep a page sized buffer around for when we need it.
	page_buffer =
	    mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	BUG_ON(page_buffer == MAP_FAILED, "unable to mmap a page.");

	mremap_dontunmap_simple();
	mremap_dontunmap_simple_shmem();
	mremap_dontunmap_simple_fixed();
	mremap_dontunmap_partial_mapping();
	mremap_dontunmap_partial_mapping_overwrite();
	run_locked_case("mlock-onfault self-merge",
			case_mlock_onfault_self_merge);
	run_locked_case("mlock partial", case_locked_partial_mlock);
	run_locked_case("mlock2 onfault partial", case_locked_partial_onfault);

	BUG_ON(munmap(page_buffer, page_size) == -1,
	       "unable to unmap page buffer");

	ksft_finished();
}
