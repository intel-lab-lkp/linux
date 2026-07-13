// SPDX-License-Identifier: GPL-2.0
/*
 * Regression test for list corruption in
 * mm/hugetlb.c:allocate_file_region_entries().
 *
 * Fork processes sharing a hugetlb inode via `memfd_create(MFD_HUGETLB)`
 * and have them fault ranges and punch holes to fragment the shared
 * `resv_map` while draining `resv->region_cache` from a separate address
 * space.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <linux/falloc.h>
#include <linux/memfd.h>

#include "../kselftest.h"
#include "hugepage_settings.h"

/* Tunables (overridable via argv). */
static unsigned long file_pages = 64;	/* huge pages backing the shared file */
static unsigned int nr_procs;		/* 0 => auto (2 * nr online cpus) */
static unsigned int duration_sec = 5;	/* --trigger run time */

static unsigned long huge_page_size;	/* bytes */
static int memfd = -1;
static volatile int *stop;		/* shared across forked children */

/*
 * Per-process SIGBUS escape: a fault can fail (pool exhausted) and raise
 * SIGBUS; jump back and abandon the rest of the current range instead of
 * dying.  Each child is single-threaded, so a file-scope buffer is fine.
 */
static sigjmp_buf sigbus_env;

static void sigbus_handler(int sig)
{
	siglongjmp(sigbus_env, 1);
}

static void install_sigbus_handler(void)
{
	struct sigaction sa = {
		.sa_handler = sigbus_handler,
	};

	sigemptyset(&sa.sa_mask);
	sigaction(SIGBUS, &sa, NULL);
}

static long sys_memfd_create(const char *name, unsigned int flags)
{
	return syscall(__NR_memfd_create, name, flags);
}

/*
 * Map [start, start+len) of the shared file and fault every page in.
 *
 * The mmap() drives mmap-time hugetlb_reserve_pages -> region_chg (the
 * wide-window victim when the range is fragmented) + region_add.  The
 * subsequent stores fault pages in *without* inode_lock, driving the
 * lock-free region_add/region_chg that drains resv->region_cache -- the
 * concurrency the buggy loop needs from *other* processes.
 */
static void map_and_fault_range(unsigned long start, unsigned long len)
{
	unsigned long i;
	char *addr;

	if (!len)
		return;
	addr = mmap(NULL, len * huge_page_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, memfd, start * huge_page_size);
	if (addr == MAP_FAILED)
		return;		/* ENOMEM under pool pressure is fine */

	for (i = 0; i < len; i++) {
		if (sigsetjmp(sigbus_env, 1) == 0)
			*(volatile char *)(addr + i * huge_page_size) = (char)i;
		else
			break;	/* SIGBUS: pool exhausted, stop faulting */
	}
	munmap(addr, len * huge_page_size);
}

/*
 * Remove [start, start+len) huge pages from the shared file, driving
 * hugetlbfs_punch_hole -> remove_inode_hugepages -> region_del for the pages
 * that were faulted in above.  This frees pages back to the pool (bounding
 * residency) and re-fragments the resv_map so later region_chg calls keep
 * needing multiple entries.
 */
static void punch_range(unsigned long start, unsigned long len)
{
	if (!len)
		return;
	fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		  start * huge_page_size, len * huge_page_size);
}

/*
 * One forked worker.  Each worker is simultaneously a victim (its wide
 * mmap-time region_chg) and, for the other workers, a lock-free cache
 * drainer (its faults) -- exactly the shared-inode map+fault churn seen in
 * the original crash (QEMU prealloc + SPDK/Bytelight on one guest-RAM file).
 */
static void child_worker(unsigned int id)
{
	unsigned int seed = id * 2654435761u ^ (unsigned int)time(NULL) ^
			    ((unsigned int)getpid() << 1);

	install_sigbus_handler();

	while (!*stop) {
		unsigned long start = rand_r(&seed) % file_pages;
		unsigned long len = 1 + rand_r(&seed) % (file_pages - start);

		/*
		 * Bias toward map+fault (victims and drainers); punch a
		 * quarter of the time to keep the map fragmented and to cap
		 * how many pages stay resident.
		 */
		if (rand_r(&seed) % 4)
			map_and_fault_range(start, len);
		else
			punch_range(start, len);
	}
}

/*
 * Safe, single-process functional check.  It fragments the shared resv_map
 * (reservations for a shared mapping persist on the inode after munmap) so
 * that a subsequent whole-file mmap makes region_chg compute
 * regions_needed > 1, forcing allocate_file_region_entries to allocate and
 * splice several entries.  This exercises the buggy line, but without
 * concurrency it always makes exactly one loop iteration, so it cannot trip
 * the race and is safe on any kernel.  Returns 0 on success.
 */
static int functional_check(void)
{
	unsigned long i;
	void *addr;

	/* Punch everything so we start from an empty resv_map. */
	punch_range(0, file_pages);

	/* Reserve every other huge page -> ~file_pages/2 disjoint regions. */
	for (i = 0; i < file_pages; i += 2)
		map_and_fault_range(i, 1);

	/*
	 * One mapping spanning the whole file must fill all the holes in a
	 * single region_chg -> a multi-entry region_cache refill.
	 */
	addr = mmap(NULL, file_pages * huge_page_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, memfd, 0);
	if (addr == MAP_FAILED) {
		ksft_print_msg("whole-file mmap failed: %s\n", strerror(errno));
		return -1;
	}
	munmap(addr, file_pages * huge_page_size);
	punch_range(0, file_pages);
	return 0;
}

static int run_trigger(void)
{
	pid_t *pids;
	unsigned int i, started = 0;

	stop = mmap(NULL, sizeof(*stop), PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (stop == MAP_FAILED)
		ksft_exit_fail_msg("mmap(stop flag): %s\n", strerror(errno));
	*stop = 0;

	pids = calloc(nr_procs, sizeof(*pids));
	if (!pids)
		ksft_exit_fail_msg("calloc: %s\n", strerror(errno));

	ksft_print_msg("stressing %u processes for %us on %lu huge pages (%lu MiB)\n",
		       nr_procs, duration_sec, file_pages,
		       (file_pages * huge_page_size) >> 20);
	ksft_print_msg("WARNING: a vulnerable kernel will panic here\n");

	for (i = 0; i < nr_procs; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			ksft_print_msg("fork: %s\n", strerror(errno));
			break;
		}
		if (pid == 0) {
			free(pids);
			child_worker(i);
			_exit(0);
		}
		pids[started++] = pid;
	}

	if (!started) {
		free(pids);
		munmap((void *)stop, sizeof(*stop));
		ksft_exit_fail_msg("no worker processes started\n");
	}

	sleep(duration_sec);
	*stop = 1;

	for (i = 0; i < started; i++)
		waitpid(pids[i], NULL, 0);
	free(pids);
	munmap((void *)stop, sizeof(*stop));
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--trigger] [-s huge_pages] [-t procs] [-d seconds]\n"
		"  (default) run a safe single-process functional check\n"
		"  --trigger fork+fault concurrent reproducer (CAN PANIC A BUGGY HOST)\n"
		"  -s  huge pages backing the shared file (default %lu)\n"
		"  -t  worker processes for --trigger (default 2 * nr_cpus)\n"
		"  -d  --trigger run time in seconds (default %u)\n",
		prog, file_pages, duration_sec);
}

int main(int argc, char *argv[])
{
	bool trigger = false;
	int i, ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--trigger")) {
			trigger = true;
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			file_pages = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			nr_procs = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			duration_sec = strtoul(argv[++i], NULL, 0);
		} else {
			usage(argv[0]);
			return KSFT_FAIL;
		}
	}

	ksft_print_header();
	ksft_set_plan(1);

	if (file_pages < 4)
		file_pages = 4;
	if (!nr_procs) {
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);

		nr_procs = cpus > 0 ? (unsigned int)cpus * 2 : 4;
	}
	if (nr_procs < 2)
		nr_procs = 2;

	huge_page_size = default_huge_page_size();
	if (!huge_page_size)
		ksft_exit_skip("no hugetlbfs / huge page size available\n");

	/*
	 * Both modes fault pages in (functional_check touches every page it
	 * maps, and --trigger's workers fault the ranges they map), so the
	 * whole file may become resident; require that many *free* huge pages,
	 * not just reservations.
	 */
	if (hugetlb_free_default_pages() < file_pages)
		ksft_exit_skip("need %lu free huge pages, have %lu (set nr_hugepages)\n",
			       file_pages, hugetlb_free_default_pages());

	memfd = sys_memfd_create("hugetlb_region_cache_race",
				 MFD_HUGETLB |
				 (__builtin_ctzl(huge_page_size) << MFD_HUGE_SHIFT));
	if (memfd < 0)
		ksft_exit_skip("memfd_create(MFD_HUGETLB): %s\n",
			       strerror(errno));
	if (ftruncate(memfd, file_pages * huge_page_size)) {
		close(memfd);
		ksft_exit_fail_msg("ftruncate: %s\n", strerror(errno));
	}

	if (trigger)
		ret = run_trigger();
	else
		ret = functional_check();

	close(memfd);

	if (ret)
		ksft_test_result_fail("hugetlb region_cache race check\n");
	else if (trigger)
		ksft_test_result_pass("survived concurrent region_cache churn\n");
	else
		ksft_test_result_pass("multi-entry region_cache refill works\n");

	ksft_finished();
}
