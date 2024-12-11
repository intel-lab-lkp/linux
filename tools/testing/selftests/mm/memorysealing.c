// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "../kselftest_harness.h"
#include <asm-generic/unistd.h>
#include <errno.h>
#include <syscall.h>
#include "memorysealing.h"

/*
 * To avoid auto-merging, create a VMA with PROT_NONE pages at each end.
 * If unsuccessful, return MAP_FAILED.
 */
static void *setup_single_address(int size, int prot)
{
	int ret;
	void *ptr;
	unsigned long page_size = getpagesize();

	ptr = mmap(NULL, size + 2 * page_size, prot,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	if (ptr == MAP_FAILED)
		return MAP_FAILED;

	/* To avoid auto-merging, change to PROT_NONE at each end. */
	ret = sys_mprotect(ptr, page_size, PROT_NONE);
	if (ret != 0)
		return MAP_FAILED;

	ret = sys_mprotect(ptr + size + page_size, page_size, PROT_NONE);
	if (ret != 0)
		return MAP_FAILED;

	return ptr + page_size;
}

FIXTURE(basic)
{
	unsigned long page_size;
	unsigned long size;
	void *ptr;
};

/*
 * Setup for basic:
 * Single VMA with 4 pages, prot = (PROT_READ | PROT_WRITE).
 */
FIXTURE_SETUP(basic)
{
	int prot;

	self->page_size = getpagesize();

	if (!mseal_supported())
		SKIP(return, "mseal is not supported");

	/* Create a single VMA with 4 pages, prot as PROT_READ | PROT_WRITE. */
	self->size = self->page_size * 4;
	self->ptr = setup_single_address(self->size, PROT_READ | PROT_WRITE);
	EXPECT_NE(self->ptr, MAP_FAILED);

	EXPECT_EQ(self->size, get_vma_size(self->ptr, &prot));
	EXPECT_EQ(PROT_READ | PROT_WRITE, prot);
};

FIXTURE_TEARDOWN(basic)
{
}

FIXTURE(two_vma)
{
	unsigned long page_size;
	unsigned long size;
	void *ptr;
};

/*
 * Setup for two_vma:
 * Two consecutive VMAs, each with 2 pages.
 * The first VMA:  prot = PROT_READ.
 * The second VMA: prot = (PROT_READ | PROT_WRITE).
 */
FIXTURE_SETUP(two_vma)
{
	int prot;
	int ret;

	self->page_size = getpagesize();

	if (!mseal_supported())
		SKIP(return, "mseal is not supported");

	/* Create a single VMA with 4 pages, prot as PROT_READ | PROT_WRITE. */
	self->size = getpagesize() * 4;
	self->ptr = setup_single_address(self->size, PROT_READ | PROT_WRITE);
	EXPECT_NE(self->ptr, MAP_FAILED);

	/* Use mprotect to split as two VMA. */
	ret = sys_mprotect(self->ptr, self->page_size * 2, PROT_READ);
	ASSERT_EQ(ret, 0);

	/* Verify the first VMA is 2 pages and prot bits */
	EXPECT_EQ(self->page_size * 2, get_vma_size(self->ptr, &prot));
	EXPECT_EQ(PROT_READ, prot);

	/* Verify the second VMA is 2 pages and prot bits */
	EXPECT_EQ(self->page_size * 2,
		get_vma_size(self->ptr + self->page_size * 2, &prot));
	EXPECT_EQ(PROT_READ | PROT_WRITE, prot);
};

FIXTURE_TEARDOWN(two_vma)
{
}

/*
 * Verify mprotect is blocked.
 */
TEST_F(basic, mprotect_basic)
{
	int ret;
	unsigned long size;
	int prot;

	/* Seal the mapping. */
	ret = sys_mseal(self->ptr, self->size, 0);
	ASSERT_EQ(ret, 0);

	/* Verify mprotect is blocked. */
	ret = sys_mprotect(self->ptr, self->size, PROT_READ);
	EXPECT_GT(0, ret);
	EXPECT_EQ(EPERM, errno);

	/* Verify the VMA (sealed) isn't changed */
	size = get_vma_size(self->ptr, &prot);
	EXPECT_EQ(self->size, size);
	EXPECT_EQ(PROT_READ | PROT_WRITE, prot);
}

/*
 * Seal both VMAs in one mseal call.
 * Verify mprotect is blocked on both VMAs in various cases.
 */
TEST_F(two_vma, mprotect)
{
	int ret;
	int prot;
	unsigned long size;

	/* Seal both VMAs in one mseal call. */
	ret = sys_mseal(self->ptr, self->size, 0);
	ASSERT_EQ(ret, 0);

	/* Verify mprotect is rejected on the first VMA. */
	ret = sys_mprotect(self->ptr, self->page_size * 2,
		PROT_READ | PROT_EXEC);
	EXPECT_GT(0, ret);
	EXPECT_EQ(EPERM, errno);

	/* Verify mprotect is rejected on the second VMA. */
	ret = sys_mprotect(self->ptr, self->page_size * 2,
		PROT_READ | PROT_EXEC);
	EXPECT_GT(0, ret);
	EXPECT_EQ(EPERM, errno);

	/* Attempt of mprotect two VMAs at the same call is blocked */
	ret = sys_mprotect(self->ptr, self->size,
		PROT_READ | PROT_EXEC);
	EXPECT_GT(0, ret);
	EXPECT_EQ(EPERM, errno);

	/* Verify both VMAs aren't changed. */
	size = get_vma_size(self->ptr, &prot);
	EXPECT_EQ(self->page_size * 2, size);
	EXPECT_EQ(PROT_READ, prot);

	size = get_vma_size(self->ptr + self->page_size * 2, &prot);
	EXPECT_EQ(self->page_size * 2, size);
	EXPECT_EQ(PROT_READ | PROT_WRITE, prot);
}

TEST_HARNESS_MAIN
