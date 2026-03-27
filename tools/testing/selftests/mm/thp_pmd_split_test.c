// SPDX-License-Identifier: GPL-2.0
/*
 * Tests various kernel code paths that handle THP PMD splitting.
 *
 * Prerequisites:
 * - THP enabled (always or madvise mode):
 *   echo always > /sys/kernel/mm/transparent_hugepage/enabled
 *   or
 *   echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#include "kselftest_harness.h"
#include "thp_settings.h"
#include "vm_util.h"

/* Read vmstat counter */
static unsigned long read_vmstat(const char *name)
{
	FILE *fp;
	char line[256];
	unsigned long value = 0;

	fp = fopen("/proc/vmstat", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, name, strlen(name)) == 0 &&
		    line[strlen(name)] == ' ') {
			sscanf(line + strlen(name), " %lu", &value);
			break;
		}
	}
	fclose(fp);
	return value;
}

/*
 * Log vmstat counters for split_pmd_after/split_pmd_failed_after,
 * check if split_pmd_after is greater than before and split_pmd_failed_after
 * hasn't incremented.
 */
static void log_and_check_pmd_split(struct __test_metadata *const _metadata,
	unsigned long split_pmd_before, unsigned long split_pmd_failed_before)
{
	unsigned long split_pmd_after = read_vmstat("thp_split_pmd");
	unsigned long split_pmd_failed_after = read_vmstat("thp_split_pmd_failed");

	TH_LOG("thp_split_pmd: %lu -> %lu", \
	       split_pmd_before, split_pmd_after);
	TH_LOG("thp_split_pmd_failed: %lu -> %lu", \
	       split_pmd_failed_before, split_pmd_failed_after);
	ASSERT_GT(split_pmd_after, split_pmd_before);
	ASSERT_EQ(split_pmd_failed_after, split_pmd_failed_before);
}

/* Allocate a THP at the given aligned address */
static int allocate_thp(void *aligned, size_t pmdsize)
{
	int ret;

	ret = madvise(aligned, pmdsize, MADV_HUGEPAGE);
	if (ret)
		return -1;

	/* Touch all pages to allocate the THP */
	memset(aligned, 0xAA, pmdsize);

	/* Verify we got a THP */
	if (!check_huge_anon(aligned, 1, pmdsize))
		return -1;

	return 0;
}

FIXTURE(thp_pmd_split)
{
	void *mem;		/* Base mmap allocation */
	void *aligned;		/* PMD-aligned pointer within mem */
	size_t pmdsize;		/* PMD size from sysfs */
	size_t pagesize;	/* Base page size */
	size_t mmap_size;	/* Total mmap size for alignment */
	unsigned long split_pmd_before;
	unsigned long split_pmd_failed_before;
};

FIXTURE_SETUP(thp_pmd_split)
{
	if (!thp_available())
		SKIP(return, "THP not available");

	self->pmdsize = read_pmd_pagesize();
	if (!self->pmdsize)
		SKIP(return, "Unable to read PMD size");

	self->pagesize = getpagesize();
	self->mmap_size = 4 * self->pmdsize;

	self->split_pmd_before = read_vmstat("thp_split_pmd");
	self->split_pmd_failed_before = read_vmstat("thp_split_pmd_failed");

	self->mem = mmap(NULL, self->mmap_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->mem, MAP_FAILED);

	/* Align to PMD boundary */
	self->aligned = (void *)(((unsigned long)self->mem + self->pmdsize - 1) &
				 ~(self->pmdsize - 1));
}

FIXTURE_TEARDOWN(thp_pmd_split)
{
	if (self->mem && self->mem != MAP_FAILED)
		munmap(self->mem, self->mmap_size);
}

/*
 * Partial munmap on THP (zap_pmd_range)
 *
 * Tests that partial munmap of a THP correctly splits the PMD.
 * This exercises zap_pmd_range part of split.
 */
TEST_F(thp_pmd_split, partial_munmap)
{
	int ret;

	ret = allocate_thp(self->aligned, self->pmdsize);
	if (ret)
		SKIP(return, "Failed to allocate THP");

	ret = munmap((char *)self->aligned + self->pagesize, self->pagesize);
	ASSERT_EQ(ret, 0);

	log_and_check_pmd_split(_metadata, self->split_pmd_before,
		self->split_pmd_failed_before);
}

/*
 * Partial mprotect on THP (change_pmd_range)
 *
 * Tests that partial mprotect of a THP correctly splits the PMD and
 * applies protection only to the requested portion. This exercises
 * the mprotect path which now handles split failures.
 */
TEST_F(thp_pmd_split, partial_mprotect)
{
	volatile unsigned char *ptr = (volatile unsigned char *)self->aligned;
	int ret;

	ret = allocate_thp(self->aligned, self->pmdsize);
	if (ret)
		SKIP(return, "Failed to allocate THP");

	/* Partial mprotect - make middle page read-only */
	ret = mprotect((char *)self->aligned + self->pagesize, self->pagesize, PROT_READ);
	ASSERT_EQ(ret, 0);

	/* Verify we can still write to non-protected pages */
	ptr[0] = 0xDD;
	ptr[self->pmdsize - 1] = 0xEE;

	ASSERT_EQ(ptr[0], (unsigned char)0xDD);
	ASSERT_EQ(ptr[self->pmdsize - 1], (unsigned char)0xEE);

	log_and_check_pmd_split(_metadata, self->split_pmd_before,
		self->split_pmd_failed_before);
}

/*
 * Partial mlock triggering split (walk_page_range)
 *
 * Tests mlock on a partial THP region which should trigger a PMD split.
 */
TEST_F(thp_pmd_split, partial_mlock)
{
	int ret;

	ret = allocate_thp(self->aligned, self->pmdsize);
	if (ret)
		SKIP(return, "Failed to allocate THP");

	/* Partial mlock - should trigger PMD split */
	ret = mlock((char *)self->aligned + self->pagesize, self->pagesize);
	if (ret && errno == ENOMEM)
		SKIP(return, "mlock failed with ENOMEM (resource limit)");
	ASSERT_EQ(ret, 0);

	/* Cleanup */
	munlock((char *)self->aligned + self->pagesize, self->pagesize);

	log_and_check_pmd_split(_metadata, self->split_pmd_before,
		self->split_pmd_failed_before);
}

/*
 * Partial mremap (move_page_tables)
 *
 * Tests that partial mremap of a THP correctly splits the PMD and
 * moves only the requested portion. This exercises move_page_tables()
 * which now handles split failures.
 */
TEST_F(thp_pmd_split, partial_mremap)
{
	void *new_addr;
	unsigned long *ptr = (unsigned long *)self->aligned;
	unsigned long *new_ptr;
	unsigned long pattern = 0xABCDUL;
	int ret;

	ret = allocate_thp(self->aligned, self->pmdsize);
	if (ret)
		SKIP(return, "Failed to allocate THP");

	/* Write pattern to the page we'll move */
	ptr[self->pagesize / sizeof(unsigned long)] = pattern;

	/* Also write to first and last page to verify they stay intact */
	ptr[0] = 0x1234UL;
	ptr[(self->pmdsize - self->pagesize) / sizeof(unsigned long)] = 0x4567UL;

	/* Partial mremap - move one base page from the THP */
	new_addr = mremap((char *)self->aligned + self->pagesize, self->pagesize,
			  self->pagesize, MREMAP_MAYMOVE);
	if (new_addr == MAP_FAILED) {
		if (errno == ENOMEM)
			SKIP(return, "mremap failed with ENOMEM");
		ASSERT_NE(new_addr, MAP_FAILED);
	}

	/* Verify data was moved correctly */
	new_ptr = (unsigned long *)new_addr;
	ASSERT_EQ(new_ptr[0], pattern);

	/* Verify surrounding data is intact */
	ASSERT_EQ(ptr[0], 0x1234UL);
	ASSERT_EQ(ptr[(self->pmdsize - self->pagesize) / sizeof(unsigned long)], 0x4567UL);

	/* Cleanup the moved page */
	munmap(new_addr, self->pagesize);

	log_and_check_pmd_split(_metadata, self->split_pmd_before,
		self->split_pmd_failed_before);
}

/*
 * MADV_DONTNEED on THP
 *
 * Tests that MADV_DONTNEED on a partial THP correctly handles
 * the PMD split and discards only the requested pages.
 */
TEST_F(thp_pmd_split, partial_madv_dontneed)
{
	volatile unsigned char *ptr = (volatile unsigned char *)self->aligned;
	int ret;

	ret = allocate_thp(self->aligned, self->pmdsize);
	if (ret)
		SKIP(return, "Failed to allocate THP");

	/* Write pattern */
	memset(self->aligned, 0xDD, self->pmdsize);

	/* Partial MADV_DONTNEED - discard middle page */
	ret = madvise((char *)self->aligned + self->pagesize, self->pagesize, MADV_DONTNEED);
	ASSERT_EQ(ret, 0);

	/* Verify non-discarded pages still have data */
	ASSERT_EQ(ptr[0], (unsigned char)0xDD);
	ASSERT_EQ(ptr[2 * self->pagesize], (unsigned char)0xDD);
	ASSERT_EQ(ptr[self->pmdsize - 1], (unsigned char)0xDD);

	/* Discarded page should be zero */
	ASSERT_EQ(ptr[self->pagesize], (unsigned char)0x00);

	log_and_check_pmd_split(_metadata, self->split_pmd_before,
		self->split_pmd_failed_before);
}

TEST_HARNESS_MAIN
