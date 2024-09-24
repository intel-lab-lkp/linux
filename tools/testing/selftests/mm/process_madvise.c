// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE
#include "../kselftest_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>

/* May not be available in host system yet. */
#ifndef PR_MADV_SELF
#define PR_MADV_SELF	(1<<0)
#endif

FIXTURE(process_madvise)
{
	unsigned long page_size;
};

FIXTURE_SETUP(process_madvise)
{
	self->page_size = (unsigned long)sysconf(_SC_PAGESIZE);
};

FIXTURE_TEARDOWN(process_madvise)
{
}

static void populate_range(char *ptr, size_t len)
{
	memset(ptr, 'x', len);
}

static bool is_range_zeroed(char *ptr, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (ptr[i] != '\0')
			return false;
	}

	return true;
}

TEST_F(process_madvise, pr_madv_self)
{
	const unsigned long page_size = self->page_size;
	struct iovec vec[3];
	char *ptr_region, *ptr, *ptr2, *ptr3;

	/* Establish a region in which to place VMAs. */
	ptr_region = mmap(NULL, 100 * page_size, PROT_NONE,
			  MAP_PRIVATE | MAP_ANON, -1, 0);
	ASSERT_NE(ptr_region, MAP_FAILED);

	/* Place a 5 page mapping offset by one page into the region. */
	ptr = mmap(&ptr_region[page_size], 5 * page_size,
		   PROT_READ | PROT_WRITE,
		   MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	populate_range(ptr, 5 * page_size);
	vec[0].iov_base = ptr;
	vec[0].iov_len = 5 * page_size;
	/* Free the PROT_NONE region before this region. */
	ASSERT_EQ(munmap(ptr_region, page_size), 0);

	/* Place a 10 page mapping in the middle of the region. */
	ptr2 = mmap(&ptr_region[50 * page_size], 10 * page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr2, MAP_FAILED);
	populate_range(ptr2, 10 * page_size);
	vec[1].iov_base = ptr2;
	vec[1].iov_len = 10 * page_size;
	/* Free the PROT_NONE region before this region. */
	ASSERT_EQ(munmap(&ptr_region[6 * page_size], 44 * page_size), 0);

	/* Place a 3 page mapping at the end of the region, offset by 1. */
	ptr3 = mmap(&ptr_region[96 * page_size], 3 * page_size,
		    PROT_READ | PROT_WRITE,
		    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	ASSERT_NE(ptr3, MAP_FAILED);
	populate_range(ptr3, 3 * page_size);
	vec[2].iov_base = ptr3;
	vec[2].iov_len = 3 * page_size;
	/* Free the PROT_NONE region before this region. */
	ASSERT_EQ(munmap(&ptr_region[60 * page_size], 36 * page_size), 0);
	/* Free the PROT_NONE region after this region. */
	ASSERT_EQ(munmap(&ptr_region[99 * page_size], page_size), 0);

	/*
	 * OK now we should have three distinct regions of memory. Zap
	 * them with MADV_DONTNEED. This should clear the populated ranges and
	 * we can then assert on them being zeroed.
	 *
	 * The function returns the number of bytes advised, so assert this is
	 * equal to the total size of the three regions.
	 */
	ASSERT_EQ(process_madvise(0, vec, 3, MADV_DONTNEED, PR_MADV_SELF),
		  (5 + 10 + 3) * page_size);

	/* Make sure these ranges are now zeroed. */
	ASSERT_TRUE(is_range_zeroed(ptr, 5 * page_size));
	ASSERT_TRUE(is_range_zeroed(ptr2, 10 * page_size));
	ASSERT_TRUE(is_range_zeroed(ptr2, 3 * page_size));

	/* Cleanup. */
	ASSERT_EQ(munmap(ptr, 5 * page_size), 0);
	ASSERT_EQ(munmap(ptr2, 10 * page_size), 0);
	ASSERT_EQ(munmap(ptr3, 3 * page_size), 0);
}

TEST_HARNESS_MAIN
