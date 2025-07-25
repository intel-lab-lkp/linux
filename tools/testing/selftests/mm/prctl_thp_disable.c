// SPDX-License-Identifier: GPL-2.0
/*
 * Basic tests for PR_GET/SET_THP_DISABLE prctl calls
 *
 * Author(s): Usama Arif <usamaarif642@gmail.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include "../kselftest_harness.h"
#include "thp_settings.h"
#include "vm_util.h"

#ifndef PR_THP_DISABLE_EXCEPT_ADVISED
#define PR_THP_DISABLE_EXCEPT_ADVISED (1 << 1)
#endif

#define NR_HUGEPAGES 6

static int sz2ord(size_t size, size_t pagesize)
{
	return __builtin_ctzll(size / pagesize);
}

enum madvise_buffer {
	NONE,
	HUGE,
	COLLAPSE
};

/*
 * Function to mmap a buffer, fault it in, madvise it appropriately (before
 * page fault for MADV_HUGE, and after for MADV_COLLAPSE), and check if the
 * mmap region is huge.
 * returns:
 * 0 if test doesn't give hugepage
 * 1 if test gives a hugepage
 * -1 if mmap fails
 */
static int test_mmap_thp(enum madvise_buffer madvise_buf, size_t pmdsize)
{
	int ret;
	int buf_size = NR_HUGEPAGES * pmdsize;

	char *buffer = (char *)mmap(NULL, buf_size, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buffer == MAP_FAILED)
		return -1;

	if (madvise_buf == HUGE)
		madvise(buffer, buf_size, MADV_HUGEPAGE);

	/* Ensure memory is allocated */
	memset(buffer, 1, buf_size);

	if (madvise_buf == COLLAPSE)
		madvise(buffer, buf_size, MADV_COLLAPSE);

	ret = check_huge_anon(buffer, NR_HUGEPAGES, pmdsize);
	munmap(buffer, buf_size);
	return ret;
}
FIXTURE(prctl_thp_disable_completely)
{
	struct thp_settings settings;
	size_t pmdsize;
};

FIXTURE_SETUP(prctl_thp_disable_completely)
{
	if (!thp_is_enabled())
		SKIP(return, "Transparent Hugepages not available\n");

	self->pmdsize = read_pmd_pagesize();
	if (!self->pmdsize)
		SKIP(return, "Unable to read PMD size\n");

	thp_read_settings(&self->settings);
	self->settings.thp_enabled = THP_MADVISE;
	self->settings.hugepages[sz2ord(self->pmdsize, getpagesize())].enabled = THP_INHERIT;
	thp_save_settings();
	thp_push_settings(&self->settings);
}

FIXTURE_TEARDOWN(prctl_thp_disable_completely)
{
	thp_restore_settings();
}

/* prctl_thp_disable_except_madvise fixture sets system THP setting to madvise */
static void prctl_thp_disable_completely(struct __test_metadata *const _metadata,
					 size_t pmdsize)
{
	int res = 0;

	res = prctl(PR_GET_THP_DISABLE, NULL, NULL, NULL, NULL);
	ASSERT_EQ(res, 1);

	/* global = madvise, process = never, we shouldn't get HPs even with madvise */
	res = test_mmap_thp(NONE, pmdsize);
	ASSERT_EQ(res, 0);

	res = test_mmap_thp(HUGE, pmdsize);
	ASSERT_EQ(res, 0);

	res = test_mmap_thp(COLLAPSE, pmdsize);
	ASSERT_EQ(res, 0);

	/* Reset to system policy */
	res =  prctl(PR_SET_THP_DISABLE, 0, NULL, NULL, NULL);
	ASSERT_EQ(res, 0);

	/* global = madvise */
	res = test_mmap_thp(NONE, pmdsize);
	ASSERT_EQ(res, 0);

	res = test_mmap_thp(HUGE, pmdsize);
	ASSERT_EQ(res, 1);

	res = test_mmap_thp(COLLAPSE, pmdsize);
	ASSERT_EQ(res, 1);
}

TEST_F(prctl_thp_disable_completely, nofork)
{
	int res = 0;

	res = prctl(PR_SET_THP_DISABLE, 1, NULL, NULL, NULL);
	ASSERT_EQ(res, 0);

	prctl_thp_disable_completely(_metadata, self->pmdsize);
}

TEST_F(prctl_thp_disable_completely, fork)
{
	int res = 0, ret = 0;
	pid_t pid;

	res = prctl(PR_SET_THP_DISABLE, 1, NULL, NULL, NULL);
	ASSERT_EQ(res, 0);

	/* Make sure prctl changes are carried across fork */
	pid = fork();
	ASSERT_GE(pid, 0);

	if (!pid)
		prctl_thp_disable_completely(_metadata, self->pmdsize);

	wait(&ret);
	if (WIFEXITED(ret))
		ret = WEXITSTATUS(ret);
	else
		ret = -EINVAL;
	ASSERT_EQ(ret, 0);
}

TEST_HARNESS_MAIN
