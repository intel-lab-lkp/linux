// SPDX-License-Identifier: GPL-2.0
/*
 * Regression test for a stale walk->action escaping walk_pmd_range() and
 * making walk_pud_range() walk the same range twice.
 *
 * The mapping is two PMDs inside one PUD. PMD 0 is populated once and left
 * alone, PMD 1 is faulted in and dropped again by a second thread. Clearing
 * PMD 1 under smaps_pte_range() makes it raise ACTION_AGAIN, and since it is
 * the last entry the stale value leaves walk_pmd_range(), so smaps accounts
 * PMD 0 twice. A kernel that does not reclaim the emptied page table never
 * clears PMD 1 and so never hits the race.
 *
 * A hit can only come from the kernel counting the same page twice, so the
 * test cannot fail spuriously.
 */
#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vm_util.h"
#include "kselftest.h"

#define NR_PMDS		2
#define NR_ROUNDS	20000
/* Cap on how much of PMD 0 to fault in, so that a large PMD stays cheap. */
#define POP_MAX		(2 * 1024 * 1024)

static char *area;
static size_t pmd_size;
static atomic_int stop;

static void *racer(void *arg)
{
	char *pmd1 = area + pmd_size;

	while (atomic_load_explicit(&stop, memory_order_acquire) == 0) {
		/* madvise() below keeps the compiler from lifting this out. */
		*pmd1 = 1;
		madvise(pmd1, pmd_size, MADV_DONTNEED);
	}
	return NULL;
}

static unsigned long smaps_rss_kb(void)
{
	char buf[1024];
	char *entry;

	entry = __get_smap_entry(area, "Rss:", buf, sizeof(buf));
	if (!entry)
		ksft_exit_fail_msg("no Rss: entry for the test mapping\n");

	return strtoul(entry, NULL, 10);
}

int main(void)
{
	unsigned long max_rss_kb, rss_kb = 0;
	size_t size, pop_size, i;
	pthread_t thread;
	char *raw;

	ksft_print_header();

	pmd_size = read_pmd_pagesize();
	if (!pmd_size)
		ksft_exit_skip("Cannot determine PMD size\n");

	if (sysconf(_SC_NPROCESSORS_ONLN) < 2)
		ksft_exit_skip("Need at least 2 CPUs to race\n");

	size = NR_PMDS * pmd_size;

	/*
	 * Align to the mapping size to stay inside one PUD, then trim the
	 * slack so that smaps has exactly one VMA to report.
	 */
	raw = mmap(NULL, 2 * size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (raw == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed\n");

	area = (char *)(((unsigned long)raw + size - 1) & ~(size - 1));
	if (area != raw)
		munmap(raw, area - raw);
	if (raw + 2 * size != area + size)
		munmap(area + size, raw + 2 * size - (area + size));

	/* A huge PMD never reaches pte_offset_map_lock(), so keep them out. */
	if (madvise(area, size, MADV_NOHUGEPAGE))
		ksft_exit_skip("MADV_NOHUGEPAGE failed\n");

	pop_size = pmd_size < POP_MAX ? pmd_size : POP_MAX;
	memset(area, 1, pop_size);

	max_rss_kb = (pop_size >> 10) + 256;

	/* Over the limit before racing means this is not our own mapping. */
	rss_kb = smaps_rss_kb();
	if (rss_kb > max_rss_kb)
		ksft_exit_fail_msg("Rss is %lu kB before racing, expected at most %lu kB\n",
				   rss_kb, max_rss_kb);

	ksft_set_plan(1);
	ksft_print_msg("racing smaps against MADV_DONTNEED, %d rounds\n",
		       NR_ROUNDS);

	if (pthread_create(&thread, NULL, racer, NULL))
		ksft_exit_fail_msg("pthread_create failed\n");

	for (i = 0; i < NR_ROUNDS; i++) {
		rss_kb = smaps_rss_kb();
		if (rss_kb > max_rss_kb)
			break;
	}

	atomic_store_explicit(&stop, 1, memory_order_release);
	pthread_join(thread, NULL);

	if (i < NR_ROUNDS) {
		ksft_print_msg("walk ran twice over the same range\n");
		ksft_test_result_fail("Rss %lu kB exceeds %lu kB, round %zu\n",
				      rss_kb, max_rss_kb, i);
	} else {
		ksft_test_result_pass("Rss within %lu kB over %d rounds\n",
				      max_rss_kb, NR_ROUNDS);
	}

	ksft_exit(i == NR_ROUNDS);

	return 0;
}
