// SPDX-License-Identifier: GPL-2.0
/*
 * khugepaged race harness.
 *
 * Runs collapse against concurrent faults, transient GUP pins
 * (gup_test), fork, mremap and MADV_DONTNEED over the same ranges, in
 * one of three driver modes:
 *
 *   stepped	khugepaged, one full pass at a time through
 *		khugepaged_full_pass(), so a step covers a known extent;
 *   free	khugepaged left to run (scan_sleep_millisecs=0), for soak;
 *   madvise	MADV_COLLAPSE in a loop.
 *
 * All anon THP orders are enabled (inherit).  Occupancy runs at both ends
 * of what mTHP collapse supports: max_ptes_none 0, where a window must be
 * fully populated, and HPAGE_PMD_NR - 1, where a window full of holes
 * collapses too.  The holes are not copied from anywhere -- they are
 * zero-filled, and re-checked under the page table lock at install time in
 * case a racing fault got there first.
 *
 * Correctness signals: every racing page must read as its pattern or
 * zero (MADV_DONTNEED), never anything else.  The faulters and the fork
 * children check that continuously, a final sweep checks it once more, plus
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
static int gup_fd = -1;
static volatile int stop;
static volatile int corrupted;

static unsigned int pattern(unsigned long page_idx)
{
	unsigned int val = (unsigned int)page_idx * 2654435761U;

	return val ? val : 1;	/* never collides with the zero-fill */
}

/* Zero means never written; anything else must be this page's pattern */
static bool page_is_corrupt(unsigned long page_idx, unsigned int *val)
{
	*val = *(unsigned int *)(region + page_idx * page_size);

	return *val && *val != pattern(page_idx);
}

static void check_page(unsigned long page_idx)
{
	unsigned int val;

	if (page_is_corrupt(page_idx, &val)) {
		corrupted = 1;
		ksft_print_msg("Corruption at page %lu: %#x != %#x\n",
			       page_idx, val, pattern(page_idx));
	}
}

static unsigned long shared_pages(void)
{
	return nr_shared_areas * hpage_pmd_size / page_size;
}

static unsigned long rand_page(unsigned int *seed)
{
	return (unsigned long)rand_r(seed) % shared_pages();
}

/* Pages left from @page_idx, so a range never reaches the mremap thread's area */
static unsigned long room_from(unsigned long page_idx, unsigned long want)
{
	unsigned long left = shared_pages() - page_idx;

	return want < left ? want : left;
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

		madvise(region + page_idx * page_size,
			room_from(page_idx, nr) * page_size, MADV_DONTNEED);
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

		unsigned long nr = room_from(page_idx, 16);

		gup.addr = (unsigned long)(region + page_idx * page_size);
		gup.size = nr * page_size;
		gup.nr_pages_per_call = nr;
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
			unsigned int val;
			int bad = 0;

			/*
			 * No stdio in the child: a thread may have held
			 * stdout's lock when we forked, and printing under an
			 * inherited lock hangs.  The parent reports what the
			 * exit status says.
			 */
			for (int i = 0; i < 16; i++)
				bad |= page_is_corrupt(rand_page(&seed), &val);
			_exit(bad);
		}
		if (pid > 0) {
			int wstatus;

			if (waitpid(pid, &wstatus, 0) < 0)
				ksft_exit_fail_perror("waitpid()");
			/* A child killed on the read counts too, not just its exit code. */
			if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus))
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

static unsigned long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000UL + tv.tv_usec / 1000;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: khugepaged_race [-d seconds] [-m stepped|free|madvise] [-z] [-a areas]\n"
		"\tWithout -m, every mode runs in turn.\n"
		"\t-d: seconds per mode (default 5)\n"
		"\tBoth occupancy limits run unless -z asks for holes only.\n"
		"\t-z: only max_ptes_none = HPAGE_PMD_NR - 1 (hole-heavy)\n"
		"\t-a: number of shared PMD-sized playground areas (default 3)\n");
	exit(1);
}

int main(int argc, char **argv)
{
	static const char * const thread_names[] = {
		"faulter", "faulter2", "dontneed", "pinner", "forker",
		"mremapper",
	};
	void *(*const thread_fns[])(void *) = {
		faulter_fn, faulter_fn, dontneed_fn, pinner_fn, forker_fn,
		mremapper_fn,
	};
	const int nr_threads = ARRAY_SIZE(thread_names);
	pthread_t threads[ARRAY_SIZE(thread_names)];
	static const char * const all_modes[] = { "stepped", "free", "madvise" };
	static const int all_nones[] = { 0, 1 };	/* strict, holes */
	const int *nones = all_nones;
	int nr_nones = ARRAY_SIZE(all_nones);
	const char *one_mode[1];
	const char * const *modes = all_modes;
	int nr_modes = ARRAY_SIZE(all_modes);
	const char *mode_arg = NULL;
	struct thp_settings settings;
	unsigned long end_ms;
	int duration_s = 5;
	unsigned long thread_mask = ~0UL;
	int nr_areas_arg = 0;
	bool holes_only = false;
	unsigned long i;
	int steps = 0;
	int opt;

	while ((opt = getopt(argc, argv, "a:d:m:t:zh")) != -1) {
		switch (opt) {
		case 'a':
			nr_areas_arg = atoi(optarg);
			break;
		case 'd':
			duration_s = atoi(optarg);
			break;
		case 'm':
			mode_arg = optarg;
			break;
		case 't':
			/* debug: bitmask of racing threads to start */
			thread_mask = strtoul(optarg, NULL, 0);
			break;
		case 'z':
			holes_only = true;
			break;
		default:
			usage();
		}
	}
	if (holes_only) {
		nones = all_nones + 1;
		nr_nones = 1;
	}

	if (mode_arg) {
		if (strcmp(mode_arg, "stepped") && strcmp(mode_arg, "free") &&
		    strcmp(mode_arg, "madvise"))
			usage();
		one_mode[0] = mode_arg;
		modes = one_mode;
		nr_modes = 1;
	}

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

	/*
	 * The mremap thread moves its area to this address and back, and
	 * MREMAP_FIXED unmaps whatever is in the way without saying so.  Claim
	 * the address here, so a layout that does not match this assumption
	 * fails now instead of losing a mapping later.  Nothing else in the
	 * process maps this low: thread stacks and malloc arenas come from the
	 * top-down mmap area, well above.
	 */
	mremap_scratch = (char *)BASE_ADDR + 2 * nr_areas * hpage_pmd_size;
	if (mmap(mremap_scratch, hpage_pmd_size, PROT_NONE,
		 MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE,
		 -1, 0) != (void *)mremap_scratch)
		ksft_exit_fail_perror("mmap() mremap scratch");

	ksft_set_plan(nr_modes * nr_nones);

	thp_save_settings();
	thp_read_settings(&settings);

	/*
	 * A base entry for the stack, so that the pop at the end of a mode
	 * always has something to write back: thp_pop_settings() on an empty
	 * stack has no settings to apply and gives up.
	 */
	thp_push_settings(&settings);

	for (int mn = 0; mn < nr_modes * nr_nones; mn++) {
		const char *mode = modes[mn / nr_nones];
		bool holes = nones[mn % nr_nones];

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
		 * Strict needs a fully populated window, which is rare under
		 * racing MADV_DONTNEED; hole-heavy windows collapse instead,
		 * so the two ends race different paths.
		 */
		settings.khugepaged.max_ptes_none = holes ?
			(hpage_pmd_size / page_size) - 1 : 0;
		settings.khugepaged.pages_to_scan =
			nr_areas * (hpage_pmd_size / page_size) * 8;
		for (i = 0; i < NR_ORDERS; i++) {
			if (thp_supported_orders() & (1UL << i))
				settings.hugepages[i].enabled = THP_INHERIT;
		}
		/* Popped at the end of this mode, before the next one. */
		thp_push_settings(&settings);

		region = mmap(BASE_ADDR, nr_areas * hpage_pmd_size,
			      PROT_READ | PROT_WRITE, MAP_ANONYMOUS |
			      MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
		if (region != BASE_ADDR)
			ksft_exit_fail_perror("mmap() playground");
		mremap_area = region + nr_shared_areas * hpage_pmd_size;

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

		ksft_test_result(!corrupted,
				 "%s/%s: %ds, %d steps, no corruption\n",
				 mode, holes ? "holes" : "strict",
				 duration_s, steps);

		/*
		 * Hand the address space and the settings back before the
		 * next mode: it maps the region at the same fixed address,
		 * and its scan cadence differs.
		 */
		munmap(region, nr_areas * hpage_pmd_size);
		thp_pop_settings();
		stop = 0;
		steps = 0;

		if (corrupted) {
			/* Memory is suspect; the rest would prove nothing. */
			while (++mn < nr_modes * nr_nones)
				ksft_test_result_skip("%s/%s: skipped after corruption\n",
						      modes[mn / nr_nones],
						      nones[mn % nr_nones] ?
						      "holes" : "strict");
			break;
		}
	}

	thp_restore_settings();
	ksft_finished();
}
