// SPDX-License-Identifier: GPL-2.0
/*
 * khugepaged race harness.
 *
 * Runs collapse against concurrent faults, transient GUP pins
 * (gup_test), fork, mremap and MADV_DONTNEED over the same ranges, in
 * one of three driver modes:
 *
 *   stepped	khugepaged, driven synchronously one full pass at a time
 *		through khugepaged_full_pass() — the primary driver: the
 *		full scan+collapse path with a deterministic extent per
 *		step;
 *   free	free-running khugepaged (scan_sleep_millisecs=0) — the
 *		soak;
 *   madvise	MADV_COLLAPSE in a loop — the legacy-PMD regression
 *		axis.
 *
 * All anon THP orders are enabled (inherit). max_ptes_none is 0 by
 * default — racing MADV_DONTNEED then steers selection across orders —
 * or the permissive limit with -z, which floods the batch engine with
 * hole and zeropage slots so the population paths (park-time zeropage
 * clear, zero-filled copy, install-time pte_none() verify and abort)
 * race the faulters directly.
 *
 * -p adds memory pressure to any of the above: MADV_PAGEOUT cycling
 * on a dedicated neighbor region (swap traffic and LRU churn; skipped
 * with a note when the host has no swap) and a compact_memory trigger
 * loop (compaction migrates source folios, racing collapse's freeze
 * with refcount elevation and migration entries of its own).
 *
 * Correctness signals: every racing page must read as its pattern or
 * zero (MADV_DONTNEED), never anything else — checked continuously by
 * the faulters and the fork children and once at the end — plus
 * whatever DEBUG_VM / page_table_check / KASAN / lockdep report in
 * dmesg, which the caller is expected to inspect.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"
#include "vm_util.h"
#include "hugepage_settings.h"
#include "../../../../mm/gup_test.h"

#define BASE_ADDR	((void *)(1UL << 30))
/*
 * Shared playground for faults/pins/fork/dontneed: several PMD-sized
 * areas the racing threads spread across, plus one area owned by the
 * mremap thread. More areas means more independent regions collapsing
 * at once; the default suits a normal machine. On a memory-constrained
 * host -- or under emulation, where a 512M PMD (arm64/64K) makes the
 * default playground multi-gigabyte -- pass -a to shrink it.
 */
#define DEFAULT_SHARED_AREAS	3
static int nr_shared_areas;
static int nr_areas;

static unsigned long hpage_pmd_size;
static unsigned long page_size;
static char *region;		/* NR_AREAS * hpage_pmd_size */
static char *mremap_area;	/* region + NR_SHARED_AREAS areas */
static char *mremap_scratch;	/* well above the region */
static char *pageout_area;	/* -p: dedicated pressure region */
static size_t pageout_size;
static int gup_fd = -1;
static volatile int stop;
static volatile int corrupted;

static unsigned int pattern(unsigned long page_idx)
{
	unsigned int val = (unsigned int)page_idx * 2654435761U;

	return val ? val : 1;	/* never collides with the zero-fill */
}

static void check_page(unsigned long page_idx)
{
	unsigned int val = *(unsigned int *)(region + page_idx * page_size);

	if (val && val != pattern(page_idx)) {
		corrupted = 1;
		ksft_print_msg("Corruption at page %lu: %#x != %#x\n",
			       page_idx, val, pattern(page_idx));
	}
}

static unsigned long rand_page(unsigned int *seed)
{
	return (unsigned long)rand_r(seed) %
	       (nr_shared_areas * hpage_pmd_size / page_size);
}

static void *faulter_fn(void *arg)
{
	unsigned int seed = (unsigned long)arg;

	while (!stop) {
		unsigned long page_idx = rand_page(&seed);

		if (rand_r(&seed) & 1)
			*(unsigned int *)(region + page_idx * page_size) =
				pattern(page_idx);
		else
			check_page(page_idx);
	}
	return NULL;
}

static void *dontneed_fn(void *arg)
{
	unsigned int seed = (unsigned long)arg;

	while (!stop) {
		unsigned long page_idx = rand_page(&seed);
		unsigned long nr = 1UL << (rand_r(&seed) % 6);	/* 1..32 pages */

		/*
		 * Once in a while zap a whole PMD-aligned area: only a
		 * zap spanning the full table triggers the empty-table
		 * reclaim (CONFIG_PT_RECLAIM), which can free a table
		 * out from under a parked collapse — sub-table zaps
		 * never reach that path.
		 */
		if (!(rand_r(&seed) % 64)) {
			unsigned long area = page_idx /
					(hpage_pmd_size / page_size);

			madvise(region + area * hpage_pmd_size,
				hpage_pmd_size, MADV_DONTNEED);
		} else {
			madvise(region + page_idx * page_size,
				nr * page_size, MADV_DONTNEED);
		}
		usleep(rand_r(&seed) % 500);
	}
	return NULL;
}

static void *pinner_fn(void *arg)
{
	unsigned int seed = (unsigned long)arg;

	while (!stop) {
		struct gup_test gup = {};
		unsigned long page_idx = rand_page(&seed);

		gup.addr = (unsigned long)(region + page_idx * page_size);
		gup.size = 16 * page_size;
		gup.nr_pages_per_call = 16;
		gup.gup_flags = 1;	/* FOLL_WRITE */
		/* Racing MADV_DONTNEED makes transient failures expected. */
		ioctl(gup_fd, PIN_FAST_BENCHMARK, &gup);
		usleep(rand_r(&seed) % 200);
	}
	return NULL;
}

static void *forker_fn(void *arg)
{
	unsigned int seed = (unsigned long)arg;

	while (!stop) {
		pid_t pid = fork();

		if (pid == 0) {
			for (int i = 0; i < 16; i++)
				check_page(rand_page(&seed));
			_exit(corrupted);
		}
		if (pid > 0) {
			int wstatus;

			waitpid(pid, &wstatus, 0);
			if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus))
				corrupted = 1;
		}
		usleep(rand_r(&seed) % 2000);
	}
	return NULL;
}

static void *mremapper_fn(void *arg)
{
	unsigned int seed = (unsigned long)arg;

	while (!stop) {
		void *p;

		p = mremap(mremap_area, hpage_pmd_size, hpage_pmd_size,
			   MREMAP_MAYMOVE | MREMAP_FIXED, mremap_scratch);
		if (p == MAP_FAILED)
			ksft_exit_fail_perror("mremap() away");
		for (int i = 0; i < 8; i++)
			mremap_scratch[(rand_r(&seed) %
				(hpage_pmd_size / page_size)) * page_size] = 1;
		p = mremap(mremap_scratch, hpage_pmd_size, hpage_pmd_size,
			   MREMAP_MAYMOVE | MREMAP_FIXED, mremap_area);
		if (p == MAP_FAILED)
			ksft_exit_fail_perror("mremap() back");
		usleep(rand_r(&seed) % 2000);
	}
	return NULL;
}

/*
 * -p: swap traffic and LRU churn on a region of our own. The content
 * check is exact — a page out and back through swap must preserve the
 * pattern, and nothing else ever writes here.
 */
static void *pageout_fn(void *arg)
{
	unsigned int seed = (unsigned long)arg;
	unsigned long nr = pageout_size / page_size;
	unsigned long i;

	for (i = 0; i < nr; i++)
		*(unsigned int *)(pageout_area + i * page_size) = pattern(i);

	while (!stop) {
		madvise(pageout_area, pageout_size, MADV_PAGEOUT);
		for (i = 0; i < nr && !stop; i++) {
			unsigned int val = *(unsigned int *)(pageout_area +
							     i * page_size);

			if (val != pattern(i)) {
				corrupted = 1;
				ksft_print_msg("Pageout corruption at page %lu: %#x != %#x\n",
					       i, val, pattern(i));
			}
		}
		usleep(rand_r(&seed) % 2000);
	}
	return NULL;
}

/* -p: compaction migrates the collapse sources out from under us. */
static void *compactor_fn(void *arg)
{
	unsigned int seed = (unsigned long)arg;
	int fd = open("/proc/sys/vm/compact_memory", O_WRONLY);

	if (fd < 0) {
		ksft_print_msg("No compact_memory; compactor idle\n");
		return NULL;
	}
	while (!stop) {
		if (write(fd, "1", 1) < 0)
			break;
		usleep(10000 + rand_r(&seed) % 100000);
	}
	close(fd);
	return NULL;
}

static bool swap_available(void)
{
	char line[256];
	int lines = 0;
	FILE *fp = fopen("/proc/swaps", "r");

	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp))
		lines++;
	fclose(fp);
	return lines > 1;
}

static unsigned long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000UL + tv.tv_usec / 1000;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: khugepaged_race [-d seconds] [-m stepped|free|madvise] [-z] [-p] [-a areas]\n"
		"\t-z: permissive max_ptes_none (hole-heavy windows)\n"
		"\t-p: memory pressure (pageout + compaction) threads\n"
		"\t-a: number of shared PMD-sized playground areas (default 3)\n");
	exit(1);
}

int main(int argc, char **argv)
{
	static const char * const thread_names[] = {
		"faulter", "faulter2", "dontneed", "pinner", "forker",
		"mremapper", "pageout", "compactor",
	};
	void *(*const thread_fns[])(void *) = {
		faulter_fn, faulter_fn, dontneed_fn, pinner_fn, forker_fn,
		mremapper_fn, pageout_fn, compactor_fn,
	};
	const unsigned long pageout_bit = 1UL << 6, compactor_bit = 1UL << 7;
	const int nr_threads = ARRAY_SIZE(thread_names);
	pthread_t threads[ARRAY_SIZE(thread_names)];
	const char *mode = "stepped";
	struct thp_settings settings;
	unsigned long end_ms;
	int duration_s = 10;
	unsigned long thread_mask = ~0UL;
	int nr_areas_arg = 0;
	bool permissive_none = false;
	bool pressure = false;
	unsigned long i;
	int steps = 0;
	int opt;

	while ((opt = getopt(argc, argv, "a:d:m:t:zph")) != -1) {
		switch (opt) {
		case 'a':
			nr_areas_arg = atoi(optarg);
			break;
		case 'd':
			duration_s = atoi(optarg);
			break;
		case 'm':
			mode = optarg;
			break;
		case 't':
			/* debug: bitmask of racing threads to start */
			thread_mask = strtoul(optarg, NULL, 0);
			break;
		case 'z':
			permissive_none = true;
			break;
		case 'p':
			pressure = true;
			break;
		default:
			usage();
		}
	}
	if (strcmp(mode, "stepped") && strcmp(mode, "free") &&
	    strcmp(mode, "madvise"))
		usage();

	ksft_print_header();
	if (!thp_available())
		ksft_exit_skip("Transparent Hugepages not available\n");

	page_size = getpagesize();
	hpage_pmd_size = read_pmd_pagesize();
	if (!hpage_pmd_size)
		ksft_exit_fail_msg("Reading PMD pagesize failed\n");

	gup_fd = open("/sys/kernel/debug/gup_test", O_RDWR);
	if (gup_fd < 0)
		ksft_exit_skip("/sys/kernel/debug/gup_test requires CONFIG_GUP_TEST and root\n");

	nr_shared_areas = nr_areas_arg > 0 ? nr_areas_arg : DEFAULT_SHARED_AREAS;
	nr_areas = nr_shared_areas + 1;

	if (!pressure) {
		thread_mask &= ~(pageout_bit | compactor_bit);
	} else if (!swap_available()) {
		/* No swap, no anon reclaim: compaction-only pressure. */
		ksft_print_msg("-p without swap: pageout thread disabled\n");
		thread_mask &= ~pageout_bit;
	}

	ksft_set_plan(1);

	thp_save_settings();
	thp_read_settings(&settings);
	settings.thp_enabled = THP_MADVISE;
	settings.thp_defrag = THP_DEFRAG_ALWAYS;
	settings.shmem_enabled = SHMEM_NEVER;
	settings.khugepaged.defrag = 1;
	settings.khugepaged.scan_sleep_millisecs =
		strcmp(mode, "free") ? 1000 : 0;
	settings.khugepaged.alloc_sleep_millisecs = 10;
	/*
	 * mTHP collapse only supports the two ends of the occupancy
	 * scale: 0 or HPAGE_PMD_NR - 1 (anything else coerces to 0).
	 * Strict is the default — it also keeps khugepaged from burning
	 * the whole step in doomed PMD-sized allocations on 512M-PMD
	 * configs, where a fully populated area is rare under racing
	 * MADV_DONTNEED. -z selects the permissive end: selection then
	 * emits hole-heavy windows and the engine's population paths
	 * take the brunt of the racing faults and zaps.
	 */
	settings.khugepaged.max_ptes_none = permissive_none ?
		(hpage_pmd_size / page_size) - 1 : 0;
	settings.khugepaged.pages_to_scan =
		nr_areas * (hpage_pmd_size / page_size) * 8;
	for (i = 0; i < NR_ORDERS; i++) {
		if (thp_supported_orders() & (1UL << i))
			settings.hugepages[i].enabled = THP_INHERIT;
	}
	/* Base of the settings stack; the bottom entry is never popped. */
	thp_push_settings(&settings);

	region = mmap(BASE_ADDR, nr_areas * hpage_pmd_size,
		      PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE,
		      -1, 0);
	if (region != BASE_ADDR)
		ksft_exit_fail_msg("Failed to allocate VMA at %p\n", BASE_ADDR);
	mremap_area = region + nr_shared_areas * hpage_pmd_size;
	mremap_scratch = (char *)BASE_ADDR + 2 * nr_areas * hpage_pmd_size;

	if (thread_mask & pageout_bit) {
		/*
		 * Big enough to cycle real reclaim, small enough not to
		 * dominate a TCG guest: 4 PMD areas, clamped to [16M, 64M].
		 */
		pageout_size = 4 * hpage_pmd_size;
		pageout_size = pageout_size < (16UL << 20) ? (16UL << 20) :
			       pageout_size > (64UL << 20) ? (64UL << 20) :
			       pageout_size;
		pageout_area = mmap(NULL, pageout_size, PROT_READ | PROT_WRITE,
				    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (pageout_area == MAP_FAILED)
			ksft_exit_fail_perror("mmap() pageout area");
	}

	/* Populate so the first pass has something to collapse. */
	for (i = 0; i < nr_shared_areas * hpage_pmd_size / page_size; i++)
		*(unsigned int *)(region + i * page_size) = pattern(i);
	memset(mremap_area, 1, hpage_pmd_size);
	madvise(region, nr_areas * hpage_pmd_size, MADV_HUGEPAGE);

	for (i = 0; i < nr_threads; i++) {
		if (!(thread_mask & (1UL << i))) {
			threads[i] = 0;
			continue;
		}
		if (pthread_create(&threads[i], NULL, thread_fns[i],
				   (void *)(i + 1)))
			ksft_exit_fail_perror("pthread_create()");
	}

	end_ms = now_ms() + duration_s * 1000UL;
	if (!strcmp(mode, "stepped")) {
		while (now_ms() < end_ms && !corrupted) {
			if (!khugepaged_full_pass(600))
				ksft_exit_fail_msg("khugepaged pass timed out\n");
			steps++;
		}
	} else if (!strcmp(mode, "free")) {
		while (now_ms() < end_ms && !corrupted)
			usleep(100 * 1000);
	} else {	/* madvise */
		while (now_ms() < end_ms && !corrupted) {
			for (i = 0; i < nr_shared_areas; i++) {
				madvise(region + i * hpage_pmd_size,
					hpage_pmd_size, MADV_COLLAPSE);
			}
			madvise(region, nr_shared_areas * hpage_pmd_size,
				MADV_DONTNEED);
			steps++;
		}
	}

	stop = 1;
	for (i = 0; i < nr_threads; i++) {
		if (threads[i])
			pthread_join(threads[i], NULL);
	}

	/* Final integrity sweep. */
	for (i = 0; i < nr_shared_areas * hpage_pmd_size / page_size; i++)
		check_page(i);

	thp_restore_settings();

	ksft_test_result(!corrupted, "%s: %ds, %d steps, no corruption\n",
			 mode, duration_s, steps);
	ksft_finished();
}
