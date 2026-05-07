// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tests for MADV_COLLAPSE behavior when the madvise range contains no
 * complete PMD-aligned window (range smaller than 2 MiB).
 *
 * madvise_collapse() rounds the caller range inward to PMD boundaries:
 *
 *   hstart = (start + ~HPAGE_PMD_MASK) & HPAGE_PMD_MASK  // round up
 *   hend   =  end   &  HPAGE_PMD_MASK                    // round down
 *
 * When hstart >= hend the collapsing loop is not entered.  Previously,
 * the final return expression computed (hend - hstart) without guarding
 * against hstart > hend, causing unsigned wrap-around and a spurious
 * -EINVAL.  Both tests expect 0: "no PMD window to collapse" is a
 * successful no-op, not an error.
 *
 * Test 1: aligned start (hstart == hend):
 *   start = 2MiB-aligned, len = PAGE_SIZE
 *   hstart = aligned, hend = aligned  ->  0 == 0  ->  0  (was already correct)
 *
 * Test 2: unaligned start (hstart > hend):
 *   start = aligned + PAGE_SIZE, len = PAGE_SIZE
 *   hstart = aligned + 2MiB, hend = aligned
 *   (hend - hstart) wraps unsigned  ->  was -EINVAL, fixed to 0
 */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/mman.h>

#include "kselftest.h"
#include "vm_util.h"

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE	25
#endif

static unsigned long	page_size;
static unsigned long	hpage_size;

/*
 * Test 1: 2MiB-aligned start, len = PAGE_SIZE.
 *   hstart == hend  ->  0
 */
static void test_subpmd_aligned(char *aligned)
{
	int ret;

	ksft_print_msg("[RUN] sub-PMD: 2MiB-aligned start, len=PAGE_SIZE\n");
	ret = madvise(aligned, page_size, MADV_COLLAPSE);
	ksft_test_result(ret == 0,
			 "sub-PMD aligned start returns 0 (ret=%d errno=%d)\n",
			 ret, ret ? errno : 0);
}

/*
 * Test 2: start = aligned + PAGE_SIZE, len = PAGE_SIZE.
 *   hstart = aligned + hpage_size  >  hend = aligned
 *   unsigned wrap was -EINVAL; correct answer is 0.
 */
static void test_subpmd_unaligned(char *aligned)
{
	int ret;

	ksft_print_msg("[RUN] sub-PMD: unaligned start (aligned+PAGE), len=PAGE_SIZE\n");
	ksft_print_msg("      hstart=%p > hend=%p\n",
		       (void *)(aligned + hpage_size), (void *)aligned);

	ret = madvise(aligned + page_size, page_size, MADV_COLLAPSE);
	if (ret && errno == EINVAL)
		ksft_print_msg("      got -EINVAL: unsigned-wrap bug not fixed\n");
	ksft_test_result(ret == 0,
			 "sub-PMD unaligned start returns 0 (ret=%d errno=%d)\n",
			 ret, ret ? errno : 0);
}

int main(void)
{
	char *base, *aligned;
	unsigned long map_size;
	int probe_ret;

	ksft_print_header();
	ksft_set_plan(2);

	page_size  = (unsigned long)getpagesize();
	hpage_size = (unsigned long)read_pmd_pagesize();
	if (!hpage_size)
		ksft_exit_skip("transparent hugepages not available\n");

	/*
	 * Probe: map one hpage-sized region, touch all pages, and attempt a
	 * real collapse to confirm MADV_COLLAPSE is supported.  EAGAIN is a
	 * transient resource failure and still counts as "available".
	 */
	map_size = 2 * hpage_size;
	base = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (base == MAP_FAILED)
		ksft_exit_fail_msg("probe mmap failed: %s\n", strerror(errno));

	aligned = (char *)(((unsigned long)base + hpage_size - 1)
			   & ~(hpage_size - 1));

	for (unsigned long i = 0; i < hpage_size; i += page_size)
		aligned[i] = 0;

	probe_ret = madvise(aligned, hpage_size, MADV_COLLAPSE);
	munmap(base, map_size);
	if (probe_ret && errno != EAGAIN)
		ksft_exit_skip("MADV_COLLAPSE not available: %s\n",
			       strerror(errno));

	/*
	 * Both sub-PMD tests share a single 2 * hpage mapping so that
	 * every test range falls within the same VMA.
	 */
	map_size = 2 * hpage_size;
	base = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (base == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));

	for (unsigned long i = 0; i < map_size; i += page_size)
		base[i] = 0;

	aligned = (char *)(((unsigned long)base + hpage_size - 1)
			   & ~(hpage_size - 1));

	test_subpmd_aligned(aligned);
	test_subpmd_unaligned(aligned);

	munmap(base, map_size);

	if (ksft_get_fail_cnt())
		ksft_exit_fail_msg("%d out of %d tests failed\n",
				   ksft_get_fail_cnt(), ksft_test_num());
	ksft_exit_pass();
}
