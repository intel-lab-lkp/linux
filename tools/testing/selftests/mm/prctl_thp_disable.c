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

static int sz2ord(size_t size, size_t pagesize)
{
	return __builtin_ctzll(size / pagesize);
}

enum thp_collapse_type {
	THP_COLLAPSE_NONE,
	THP_COLLAPSE_MADV_HUGEPAGE,	/* MADV_HUGEPAGE before access */
	THP_COLLAPSE_MADV_COLLAPSE,	/* MADV_COLLAPSE after access */
};

enum thp_policy {
	THP_POLICY_NEVER,
	THP_POLICY_MADVISE,
	THP_POLICY_ALWAYS,
};

struct test_results {
	int prctl_get_thp_disable;
	int prctl_applied_collapse_none;
	int prctl_applied_collapse_madv_huge;
	int prctl_applied_collapse_madv_collapse;
	int prctl_removed_collapse_none;
	int prctl_removed_collapse_madv_huge;
	int prctl_removed_collapse_madv_collapse;
};

/*
 * Function to mmap a buffer, fault it in, madvise it appropriately (before
 * page fault for MADV_HUGE, and after for MADV_COLLAPSE), and check if the
 * mmap region is huge.
 * Returns:
 * 0 if test doesn't give hugepage
 * 1 if test gives a hugepage
 * -errno if mmap fails
 */
static int test_mmap_thp(enum thp_collapse_type madvise_buf, size_t pmdsize)
{
	char *mem, *mmap_mem;
	size_t mmap_size;
	int ret;

	/* For alignment purposes, we need twice the THP size. */
	mmap_size = 2 * pmdsize;
	mmap_mem = (char *)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mmap_mem == MAP_FAILED)
		return -errno;

	/* We need a THP-aligned memory area. */
	mem = (char *)(((uintptr_t)mmap_mem + pmdsize) & ~(pmdsize - 1));

	if (madvise_buf == THP_COLLAPSE_MADV_HUGEPAGE)
		madvise(mem, pmdsize, MADV_HUGEPAGE);

	/* Ensure memory is allocated */
	memset(mem, 1, pmdsize);

	if (madvise_buf == THP_COLLAPSE_MADV_COLLAPSE)
		madvise(mem, pmdsize, MADV_COLLAPSE);

	/*
	 * MADV_HUGEPAGE will create a new VMA at "mem", which is the address
	 * pattern we want to check for to detect the presence of hugepage in
	 * smaps.
	 * MADV_COLLAPSE will not create a new VMA, therefore we need to check
	 * for hugepage at "mmap_mem" in smaps.
	 * Check for hugepage at both locations to ensure that
	 * THP_COLLAPSE_NONE, THP_COLLAPSE_MADV_HUGEPAGE and
	 * THP_COLLAPSE_MADV_COLLAPSE only gives a THP when expected
	 * in the range [mmap_mem, mmap_mem + 2 * pmdsize].
	 */
	ret = check_huge_anon(mem, 1, pmdsize) ||
	      check_huge_anon(mmap_mem, 1, pmdsize);
	munmap(mmap_mem, mmap_size);
	return ret;
}

static void prctl_thp_disable_test(struct __test_metadata *const _metadata,
				   size_t pmdsize, struct test_results *results)
{

	ASSERT_EQ(prctl(PR_GET_THP_DISABLE, NULL, NULL, NULL, NULL),
		  results->prctl_get_thp_disable);

	/* tests after prctl overrides global policy */
	ASSERT_EQ(test_mmap_thp(THP_COLLAPSE_NONE, pmdsize),
		  results->prctl_applied_collapse_none);

	ASSERT_EQ(test_mmap_thp(THP_COLLAPSE_MADV_HUGEPAGE, pmdsize),
		  results->prctl_applied_collapse_madv_huge);

	ASSERT_EQ(test_mmap_thp(THP_COLLAPSE_MADV_COLLAPSE, pmdsize),
		  results->prctl_applied_collapse_madv_collapse);

	/* Reset to global policy */
	ASSERT_EQ(prctl(PR_SET_THP_DISABLE, 0, NULL, NULL, NULL), 0);

	/* tests after prctl is cleared, and only global policy is effective */
	ASSERT_EQ(test_mmap_thp(THP_COLLAPSE_NONE, pmdsize),
		  results->prctl_removed_collapse_none);

	ASSERT_EQ(test_mmap_thp(THP_COLLAPSE_MADV_HUGEPAGE, pmdsize),
		  results->prctl_removed_collapse_madv_huge);

	ASSERT_EQ(test_mmap_thp(THP_COLLAPSE_MADV_COLLAPSE, pmdsize),
		  results->prctl_removed_collapse_madv_collapse);
}

FIXTURE(prctl_thp_disable_completely)
{
	struct thp_settings settings;
	struct test_results results;
	size_t pmdsize;
};

FIXTURE_VARIANT(prctl_thp_disable_completely)
{
	enum thp_policy thp_global_policy;
};

FIXTURE_VARIANT_ADD(prctl_thp_disable_completely, never)
{
	.thp_global_policy = THP_POLICY_NEVER,
};

FIXTURE_VARIANT_ADD(prctl_thp_disable_completely, madvise)
{
	.thp_global_policy = THP_POLICY_MADVISE,
};

FIXTURE_VARIANT_ADD(prctl_thp_disable_completely, always)
{
	.thp_global_policy = THP_POLICY_ALWAYS,
};

FIXTURE_SETUP(prctl_thp_disable_completely)
{
	if (!thp_available())
		SKIP(return, "Transparent Hugepages not available\n");

	self->pmdsize = read_pmd_pagesize();
	if (!self->pmdsize)
		SKIP(return, "Unable to read PMD size\n");

	thp_save_settings();
	thp_read_settings(&self->settings);
	switch (variant->thp_global_policy) {
	case THP_POLICY_NEVER:
		self->settings.thp_enabled = THP_NEVER;
		self->results = (struct test_results) {
			.prctl_get_thp_disable = 1,
			.prctl_applied_collapse_none = 0,
			.prctl_applied_collapse_madv_huge = 0,
			.prctl_applied_collapse_madv_collapse = 0,
			.prctl_removed_collapse_none = 0,
			.prctl_removed_collapse_madv_huge = 0,
			.prctl_removed_collapse_madv_collapse = 1,
		};
		break;
	case THP_POLICY_MADVISE:
		self->settings.thp_enabled = THP_MADVISE;
		self->results = (struct test_results) {
			.prctl_get_thp_disable = 1,
			.prctl_applied_collapse_none = 0,
			.prctl_applied_collapse_madv_huge = 0,
			.prctl_applied_collapse_madv_collapse = 0,
			.prctl_removed_collapse_none = 0,
			.prctl_removed_collapse_madv_huge = 1,
			.prctl_removed_collapse_madv_collapse = 1,
		};
		break;
	case THP_POLICY_ALWAYS:
		self->settings.thp_enabled = THP_ALWAYS;
		self->results = (struct test_results) {
			.prctl_get_thp_disable = 1,
			.prctl_applied_collapse_none = 0,
			.prctl_applied_collapse_madv_huge = 0,
			.prctl_applied_collapse_madv_collapse = 0,
			.prctl_removed_collapse_none = 1,
			.prctl_removed_collapse_madv_huge = 1,
			.prctl_removed_collapse_madv_collapse = 1,
		};
		break;
	}
	self->settings.hugepages[sz2ord(self->pmdsize, getpagesize())].enabled = THP_INHERIT;
	thp_write_settings(&self->settings);
}

FIXTURE_TEARDOWN(prctl_thp_disable_completely)
{
	thp_restore_settings();
}

TEST_F(prctl_thp_disable_completely, nofork)
{
	ASSERT_EQ(prctl(PR_SET_THP_DISABLE, 1, NULL, NULL, NULL), 0);
	prctl_thp_disable_test(_metadata, self->pmdsize, &self->results);
}

TEST_F(prctl_thp_disable_completely, fork)
{
	int ret = 0;
	pid_t pid;

	ASSERT_EQ(prctl(PR_SET_THP_DISABLE, 1, NULL, NULL, NULL), 0);

	/* Make sure prctl changes are carried across fork */
	pid = fork();
	ASSERT_GE(pid, 0);

	if (!pid)
		prctl_thp_disable_test(_metadata, self->pmdsize, &self->results);

	wait(&ret);
	if (WIFEXITED(ret))
		ret = WEXITSTATUS(ret);
	else
		ret = -EINVAL;
	ASSERT_EQ(ret, 0);
}

TEST_HARNESS_MAIN
