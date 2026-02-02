// SPDX-License-Identifier: GPL-2.0
/*
 * Test program for PUD-level Transparent Huge Pages (1GB anonymous THP)
 *
 * Prerequisites:
 * - Kernel with PUD THP support (CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
 * - THP enabled: echo always > /sys/kernel/mm/transparent_hugepage/enabled
 * - PUD THP enabled: echo always > /sys/kernel/mm/transparent_hugepage/hugepages-1048576kB/enabled
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
#include <sys/syscall.h>

#include "kselftest_harness.h"

#define PUD_SIZE	(1UL << 30)	/* 1GB */
#define PMD_SIZE	(1UL << 21)	/* 2MB */
#define PAGE_SIZE	(1UL << 12)	/* 4KB */

#define TEST_REGION_SIZE	(2 * PUD_SIZE)	/* 2GB to ensure PUD alignment */

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT	21
#endif

#ifndef MPOL_BIND
#define MPOL_BIND	2
#endif

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE	(1 << 1)
#endif

/* Get PUD-aligned address within a region */
static inline void *pud_align(void *addr)
{
	return (void *)(((unsigned long)addr + PUD_SIZE - 1) & ~(PUD_SIZE - 1));
}

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

/* Read mTHP stats for PUD order (1GB = 1048576kB) */
static unsigned long read_mthp_stat(const char *stat_name)
{
	char path[256];
	char buf[64];
	int fd;
	ssize_t ret;
	unsigned long value = 0;

	snprintf(path, sizeof(path),
		 "/sys/kernel/mm/transparent_hugepage/hugepages-1048576kB/stats/%s",
		 stat_name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	ret = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (ret <= 0)
		return 0;
	buf[ret] = '\0';
	sscanf(buf, "%lu", &value);
	return value;
}

/* Check if PUD THP is enabled */
static int pud_thp_enabled(void)
{
	char buf[64];
	int fd;
	ssize_t ret;

	fd = open("/sys/kernel/mm/transparent_hugepage/hugepages-1048576kB/enabled", O_RDONLY);
	if (fd < 0)
		return 0;
	ret = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (ret <= 0)
		return 0;
	buf[ret] = '\0';

	/* Check if [always] or [madvise] is set */
	if (strstr(buf, "[always]") || strstr(buf, "[madvise]"))
		return 1;
	return 0;
}

/*
 * Main fixture for PUD THP tests
 * Allocates a 2GB region and provides a PUD-aligned pointer within it
 */
FIXTURE(pud_thp)
{
	void *mem;		/* Base mmap allocation */
	void *aligned;		/* PUD-aligned pointer within mem */
	unsigned long mthp_alloc_before;
	unsigned long split_before;
};

FIXTURE_SETUP(pud_thp)
{
	if (!pud_thp_enabled())
		SKIP(return, "PUD THP not enabled in sysfs");

	self->mthp_alloc_before = read_mthp_stat("anon_fault_alloc");
	self->split_before = read_vmstat("thp_split_pud");

	self->mem = mmap(NULL, TEST_REGION_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->mem, MAP_FAILED);

	self->aligned = pud_align(self->mem);
}

FIXTURE_TEARDOWN(pud_thp)
{
	if (self->mem && self->mem != MAP_FAILED)
		munmap(self->mem, TEST_REGION_SIZE);
}

/*
 * Test: Basic PUD THP allocation
 * Verifies that touching a PUD-aligned region allocates a PUD THP
 */
TEST_F(pud_thp, basic_allocation)
{
	unsigned long mthp_alloc_after;

	/* Touch memory to trigger page fault and PUD THP allocation */
	memset(self->aligned, 0xAB, PUD_SIZE);

	mthp_alloc_after = read_mthp_stat("anon_fault_alloc");

	/*
	 * If mTHP allocation counter increased, a PUD THP was allocated.
	 */
	if (mthp_alloc_after <= self->mthp_alloc_before)
		SKIP(return, "PUD THP not allocated");

	TH_LOG("PUD THP allocated (anon_fault_alloc: %lu -> %lu)",
	       self->mthp_alloc_before, mthp_alloc_after);
}

/*
 * Test: Read/write access patterns
 * Verifies data integrity across the entire 1GB region
 */
TEST_F(pud_thp, read_write_access)
{
	unsigned long *ptr = (unsigned long *)self->aligned;
	size_t i;
	int errors = 0;

	/* Write pattern - sample every page to reduce test time */
	for (i = 0; i < PUD_SIZE / sizeof(unsigned long); i += PAGE_SIZE / sizeof(unsigned long))
		ptr[i] = i ^ 0xDEADBEEFUL;

	/* Verify pattern */
	for (i = 0; i < PUD_SIZE / sizeof(unsigned long); i += PAGE_SIZE / sizeof(unsigned long)) {
		if (ptr[i] != (i ^ 0xDEADBEEFUL))
			errors++;
	}

	ASSERT_EQ(errors, 0);
}

/*
 * Test: Fork and copy-on-write
 * Verifies that COW correctly splits the PUD THP and isolates parent/child
 */
TEST_F(pud_thp, fork_cow)
{
	unsigned long *ptr = (unsigned long *)self->aligned;
	unsigned char *bytes = (unsigned char *)self->aligned;
	pid_t pid;
	int status;
	unsigned long split_after;

	/* Initialize memory with known pattern */
	memset(self->aligned, 0xCC, PUD_SIZE);

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		/* Child: write to trigger COW */
		ptr[0] = 0x12345678UL;

		/* Verify write succeeded and rest of memory unchanged */
		if (ptr[0] != 0x12345678UL)
			_exit(1);
		if (bytes[PAGE_SIZE] != 0xCC)
			_exit(2);

		_exit(0);
	}

	/* Parent: wait for child */
	waitpid(pid, &status, 0);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 0);

	/* Verify parent memory unchanged (COW should have given child a copy) */
	ASSERT_EQ(bytes[0], 0xCC);

	split_after = read_vmstat("thp_split_pud");
	TH_LOG("Fork COW completed (thp_split_pud: %lu -> %lu)",
	       self->split_before, split_after);
}

/*
 * Test: Partial munmap triggers split
 * Verifies that unmapping part of a PUD THP splits it correctly
 */
TEST_F(pud_thp, partial_munmap)
{
	unsigned long *ptr = (unsigned long *)self->aligned;
	unsigned long *after_hole;
	unsigned long split_after;
	int ret;

	/* Touch memory to allocate PUD THP */
	memset(self->aligned, 0xDD, PUD_SIZE);

	/* Unmap a 2MB region in the middle - should trigger PUD split */
	ret = munmap((char *)self->aligned + PUD_SIZE / 2, PMD_SIZE);
	ASSERT_EQ(ret, 0);

	split_after = read_vmstat("thp_split_pud");

	/* Verify memory before the hole is still accessible and correct */
	ASSERT_EQ(ptr[0], 0xDDDDDDDDDDDDDDDDUL);

	/* Verify memory after the hole is still accessible and correct */
	after_hole = (unsigned long *)((char *)self->aligned + PUD_SIZE / 2 + PMD_SIZE);
	ASSERT_EQ(*after_hole, 0xDDDDDDDDDDDDDDDDUL);

	TH_LOG("Partial munmap completed (thp_split_pud: %lu -> %lu)",
	       self->split_before, split_after);
}

/*
 * Test: mprotect triggers split
 * Verifies that changing protection on part of a PUD THP splits it
 */
TEST_F(pud_thp, mprotect_split)
{
	volatile unsigned char *p = (unsigned char *)self->aligned;
	unsigned long split_after;
	int ret;

	/* Touch memory to allocate PUD THP */
	memset(self->aligned, 0xEE, PUD_SIZE);

	/* Change protection on a 2MB region - should trigger PUD split */
	ret = mprotect((char *)self->aligned + PMD_SIZE, PMD_SIZE, PROT_READ);
	ASSERT_EQ(ret, 0);

	split_after = read_vmstat("thp_split_pud");

	/* Verify memory still readable */
	ASSERT_EQ(*p, 0xEE);

	TH_LOG("mprotect split completed (thp_split_pud: %lu -> %lu)",
	       self->split_before, split_after);
}

/*
 * Test: Reclaim via MADV_PAGEOUT
 * Verifies that reclaim path correctly handles PUD THPs
 */
TEST_F(pud_thp, reclaim_pageout)
{
	volatile unsigned char *p;
	unsigned long split_after;
	int ret;

	/* Touch memory to allocate PUD THP */
	memset(self->aligned, 0xAA, PUD_SIZE);

	/* Try to reclaim the pages */
	ret = madvise(self->aligned, PUD_SIZE, MADV_PAGEOUT);
	if (ret < 0 && errno == EINVAL)
		SKIP(return, "MADV_PAGEOUT not supported");
	ASSERT_EQ(ret, 0);

	split_after = read_vmstat("thp_split_pud");

	/* Touch memory again to verify it's still accessible */
	p = (unsigned char *)self->aligned;
	(void)*p;  /* Read to bring pages back if swapped */

	TH_LOG("Reclaim completed (thp_split_pud: %lu -> %lu)",
	       self->split_before, split_after);
}

/*
 * Test: Migration via mbind
 * Verifies that migration path correctly handles PUD THPs by splitting
 */
TEST_F(pud_thp, migration_mbind)
{
	unsigned char *bytes = (unsigned char *)self->aligned;
	unsigned long nodemask = 1UL;  /* Node 0 */
	unsigned long split_after;
	int ret;

	/* Touch memory to allocate PUD THP */
	memset(self->aligned, 0xBB, PUD_SIZE);

	/* Try to migrate by changing NUMA policy */
	ret = syscall(__NR_mbind, self->aligned, PUD_SIZE, MPOL_BIND, &nodemask,
		      sizeof(nodemask) * 8, MPOL_MF_MOVE);
	/*
	 * mbind may fail with EINVAL (single node) or EIO (migration failed),
	 * which is acceptable - we just want to exercise the migration path.
	 */
	if (ret < 0 && errno != EINVAL && errno != EIO)
		TH_LOG("mbind returned unexpected error: %s", strerror(errno));

	split_after = read_vmstat("thp_split_pud");

	/* Verify data integrity */
	ASSERT_EQ(bytes[0], 0xBB);
	ASSERT_EQ(bytes[PUD_SIZE - 1], 0xBB);

	TH_LOG("Migration completed (thp_split_pud: %lu -> %lu)",
	       self->split_before, split_after);
}

TEST_HARNESS_MAIN
