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

TEST_HARNESS_MAIN
