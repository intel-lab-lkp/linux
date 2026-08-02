// SPDX-License-Identifier: GPL-2.0
/*
 * Synchronous khugepaged driving check.
 *
 * Race tests drive khugepaged through the existing sysfs controls: a
 * store to scan_sleep_millisecs wakes the daemon, and full_scans
 * advancing by two is a completion barrier for one full pass that
 * started after setup (khugepaged_full_pass()). Verify the pair gives
 * deterministic, attributable results: one barrier step over one
 * prepared window produces exactly one collapse attempt on that
 * window's source pages — mm_collapse_huge_page_isolate events
 * filtered by source PFN and order — and the window is collapsed
 * afterwards, repeatably.
 *
 * scan_sleep_millisecs is set to 60s to prove the wake path: without
 * the wake, one barrier step would sleep multiples of that and blow
 * the timeout. It also keeps the daemon from free-running between
 * steps, per the khugepaged_full_pass() discipline.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kselftest.h"
#include "vm_util.h"
#include "hugepage_settings.h"

#define BASE_ADDR ((void *)(1UL << 30))
#define TARGET_ORDER 2	/* smallest order khugepaged considers */
#define NR_ITERATIONS 5

static int pagemap_fd;
static int kpageflags_fd;
static unsigned long hpage_pmd_size;

/*
 * Count collapse attempts attributable to our window: legacy-engine
 * isolate events whose scan_pfn is one of the window's source PFNs,
 * plus batch-engine per-candidate install events at the window's
 * address. Either engine reports exactly once per attempt.
 */
static int count_attributed(unsigned long *pfns, int nr_pfns,
			    unsigned long addr, unsigned int order)
{
	char line[1024];
	int count = 0;
	FILE *fp;

	fp = tracing_open_trace();
	if (!fp)
		ksft_exit_fail_msg("Cannot open trace buffer\n");

	while (fgets(line, sizeof(line), fp)) {
		char *s;
		unsigned long val;
		unsigned int ord;
		char *o;
		int i;

		s = strstr(line, "mm_collapse_huge_page_isolate:");
		if (s) {
			if (sscanf(s, "mm_collapse_huge_page_isolate: scan_pfn=0x%lx",
				   &val) != 1)
				continue;
			o = strstr(s, "order=");
			if (!o || sscanf(o, "order=%u", &ord) != 1 ||
			    ord != order)
				continue;
			for (i = 0; i < nr_pfns; i++) {
				if (val == pfns[i]) {
					count++;
					break;
				}
			}
			continue;
		}

		s = strstr(line, "mm_collapse_candidate:");
		if (s) {
			if (!strstr(s, "pass=install") ||
			    !strstr(s, "result=succeeded"))
				continue;
			o = strstr(s, "addr=");
			if (!o || sscanf(o, "addr=0x%lx", &val) != 1 ||
			    val != addr)
				continue;
			o = strstr(s, "order=");
			if (!o || sscanf(o, "order=%u", &ord) != 1 ||
			    ord != order)
				continue;
			count++;
		}
	}
	fclose(fp);
	return count;
}

static void one_step(int iteration)
{
	const size_t window = getpagesize() << TARGET_ORDER;
	const int nr_pages = 1 << TARGET_ORDER;
	unsigned long pfns[1 << TARGET_ORDER];
	bool collapsed;
	int attributed;
	char *p;
	int i;

	p = mmap(BASE_ADDR, hpage_pmd_size, PROT_READ | PROT_WRITE,
		 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (p != BASE_ADDR)
		ksft_exit_fail_msg("Failed to allocate VMA at %p\n",
				   BASE_ADDR);

	/* Prepare one window; record its source PFNs. */
	for (i = 0; i < nr_pages; i++) {
		p[i * getpagesize()] = i + 1;
		pfns[i] = pagemap_get_pfn(pagemap_fd, p + i * getpagesize());
		if (pfns[i] == -1UL)
			ksft_exit_fail_msg("Source page not present\n");
	}

	if (tracing_events_start("huge_memory"))
		ksft_exit_fail_msg("Cannot enable huge_memory events\n");

	madvise(p, hpage_pmd_size, MADV_HUGEPAGE);
	/* Wait up to 120 seconds for the pass to complete. */
	if (!khugepaged_full_pass(120))
		ksft_exit_fail_msg("khugepaged did not complete a full pass\n");

	tracing_events_stop("huge_memory");

	collapsed = is_range_backed_by_folio_orders(p, window, TARGET_ORDER,
						    pagemap_fd, kpageflags_fd);
	attributed = count_attributed(pfns, nr_pages, (unsigned long)p,
				      TARGET_ORDER);

	ksft_test_result(collapsed && attributed == 1,
			 "step %d: window collapsed, %d attributed result(s)\n",
			 iteration, attributed);

	munmap(p, hpage_pmd_size);
}

int main(void)
{
	struct thp_settings settings;
	int i;

	ksft_print_header();

	if (!thp_available())
		ksft_exit_skip("Transparent Hugepages not available\n");
	if (!(thp_supported_orders() & (1UL << TARGET_ORDER)))
		ksft_exit_skip("Order %d is not a supported anon THP order\n",
			       TARGET_ORDER);

	hpage_pmd_size = read_pmd_pagesize();
	if (!hpage_pmd_size)
		ksft_exit_fail_msg("Reading PMD pagesize failed\n");
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open(\"/proc/self/pagemap\")");
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
	if (kpageflags_fd < 0)
		ksft_exit_skip("open(\"/proc/kpageflags\") requires root\n");
	if (tracing_events_start("huge_memory"))
		ksft_exit_skip("tracefs unavailable\n");
	tracing_events_stop("huge_memory");

	ksft_set_plan(NR_ITERATIONS);

	thp_save_settings();
	thp_read_settings(&settings);
	settings.thp_enabled = THP_MADVISE;
	settings.thp_defrag = THP_DEFRAG_ALWAYS;
	settings.khugepaged.defrag = 1;
	settings.khugepaged.scan_sleep_millisecs = 60000;
	settings.khugepaged.alloc_sleep_millisecs = 60000;
	settings.khugepaged.max_ptes_none = (hpage_pmd_size / getpagesize()) - 1;
	/* One wake must complete one full pass; see khugepaged_full_pass(). */
	settings.khugepaged.pages_to_scan = 1UL << 24;
	for (i = 0; i < NR_ORDERS; i++)
		settings.hugepages[i].enabled = THP_NEVER;
	settings.hugepages[TARGET_ORDER].enabled = THP_INHERIT;
	/* Base of the settings stack; the bottom entry is never popped. */
	thp_push_settings(&settings);

	for (i = 0; i < NR_ITERATIONS; i++)
		one_step(i);

	thp_restore_settings();

	ksft_finished();
}
