// SPDX-License-Identifier: GPL-2.0
/*
 * The parent process creates a shmem and forks. The child creates a THP on the
 * mapping, fills all pages with known patterns, and then continuously verifies
 * non-punched pages. The parent punches holes via MADV_REMOVE on the shmem
 * while the child reads.
 *
 * It tests the race condition between folio_split() and filemap_get_entry(),
 * where the hole punches on shmem lead to folio_split() and reading the shmem
 * lead to filemap_get_entry().
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/mman.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <sys/prctl.h>
#include "vm_util.h"
#include "kselftest.h"
#include "thp_settings.h"

uint64_t page_size;
uint64_t pmd_pagesize;
#define NR_PMD_PAGE 5
#define FILE_SIZE (pmd_pagesize * NR_PMD_PAGE)
#define TOTAL_PAGES (FILE_SIZE / page_size)

/* Every N-th to N+M-th pages are punched; not aligned with huge page boundaries. */
#define PUNCH_INTERVAL 50 /* N */
#define PUNCH_SIZE_FACTOR 3 /* M */

#define NUM_READER_THREADS 16
#define FILL_BYTE 0xAF
#define NUM_ITERATIONS 100

#define CHILD_READY 1
#define CHILD_FAILED 2
/* Shared control block: MAP_SHARED anonymous so parent and child see same values. */
struct shared_ctl {
	atomic_uint_fast32_t ready;
	atomic_uint_fast32_t stop;
	atomic_size_t child_failures;
	atomic_size_t child_verified;
};

static int get_errno(void)
{
	return errno;
}

static void fill_page(unsigned char *base, size_t page_idx)
{
	unsigned char *page_ptr = base + page_idx * page_size;
	uint64_t idx = (uint64_t)page_idx;

	memset(page_ptr, FILL_BYTE, page_size);
	memcpy(page_ptr, &idx, sizeof(idx));
}

/* Returns true if valid, false if corrupted. */
static bool check_page(unsigned char *base, size_t page_idx)
{
	unsigned char *page_ptr = base + page_idx * page_size;
	uint64_t expected_idx = (uint64_t)page_idx;
	uint64_t got_idx;

	memcpy(&got_idx, page_ptr, 8);

	if (got_idx != expected_idx) {
		size_t off;
		int all_zero = 1;

		for (off = 0; off < page_size; off++) {
			if (page_ptr[off] != 0) {
				all_zero = 0;
				break;
			}
		}
		if (all_zero) {
			ksft_print_msg(
				"CORRUPTED: page %zu (huge page %zu) is ALL ZEROS\n",
				page_idx,
				(page_idx * page_size) / pmd_pagesize);
		} else {
			ksft_print_msg(
				"CORRUPTED: page %zu (huge page %zu): expected idx %zu, got %lu\n",
				page_idx, (page_idx * page_size) / pmd_pagesize,
				page_idx, (unsigned long)got_idx);
		}
		return false;
	}
	return true;
}

struct reader_arg {
	unsigned char *base;
	struct shared_ctl *ctl;
	int tid;
	atomic_size_t *failures;
	atomic_size_t *verified;
};

static void *reader_thread(void *arg)
{
	struct reader_arg *ra = (struct reader_arg *)arg;
	unsigned char *base = ra->base;
	struct shared_ctl *ctl = ra->ctl;
	int tid = ra->tid;
	atomic_size_t *failures = ra->failures;
	atomic_size_t *verified = ra->verified;
	size_t page_idx;

	while (atomic_load_explicit(&ctl->stop, memory_order_acquire) == 0) {
		for (page_idx = (size_t)tid; page_idx < TOTAL_PAGES;
		     page_idx += NUM_READER_THREADS) {
			if (page_idx % PUNCH_INTERVAL >= 0 &&
			    page_idx % PUNCH_INTERVAL < PUNCH_SIZE_FACTOR)
				continue;
			if (check_page(base, page_idx))
				atomic_fetch_add_explicit(verified, 1,
							  memory_order_relaxed);
			else
				atomic_fetch_add_explicit(failures, 1,
							  memory_order_relaxed);
		}
		if (atomic_load_explicit(failures, memory_order_relaxed) > 0)
			break;
	}

	return NULL;
}

static void child_reader_loop(unsigned char *base, struct shared_ctl *ctl)
{
	pthread_t threads[NUM_READER_THREADS];
	struct reader_arg args[NUM_READER_THREADS];
	atomic_size_t failures = 0;
	atomic_size_t verified = 0;
	size_t page_idx;
	size_t recheck = 0;
	int i;

	for (i = 0; i < NUM_READER_THREADS; i++) {
		args[i].base = base;
		args[i].ctl = ctl;
		args[i].tid = i;
		args[i].failures = &failures;
		args[i].verified = &verified;
		if (pthread_create(&threads[i], NULL, reader_thread,
				   &args[i]) != 0)
			ksft_exit_fail_msg("pthread_create failed\n");
	}

	for (i = 0; i < NUM_READER_THREADS; i++)
		pthread_join(threads[i], NULL);

	/* Post-sleep recheck */
	usleep(1000); /* 1 ms */

	for (page_idx = 0; page_idx < TOTAL_PAGES; page_idx++) {
		if (page_idx % PUNCH_INTERVAL >= 0 &&
		    page_idx % PUNCH_INTERVAL < PUNCH_SIZE_FACTOR)
			continue;
		if (!check_page(base, page_idx))
			recheck++;
	}
	if (recheck)
		ksft_print_msg("post-sleep failures: %zu\n", recheck);

	atomic_store_explicit(&ctl->child_failures,
			      atomic_load_explicit(&failures,
						   memory_order_relaxed),
			      memory_order_release);
	atomic_store_explicit(&ctl->child_verified,
			      atomic_load_explicit(&verified,
						   memory_order_relaxed),
			      memory_order_release);
}

/* Returns number of corrupted pages. */
static size_t verify_pages(unsigned char *base, const bool *is_punched)
{
	size_t failures = 0;
	size_t page_idx;
	size_t non_punched = 0;

	for (page_idx = 0; page_idx < TOTAL_PAGES; page_idx++) {
		if (is_punched[page_idx])
			continue;
		if (!check_page(base, page_idx)) {
			failures++;
			if (failures >= 100)
				return failures;
		}
		non_punched++;
	}
	if (failures)
		ksft_print_msg("  %zu non-punched pages are corrupted!\n",
			failures);
	return failures;
}

/* Run a single iteration. Returns total number of corrupted pages. */
static size_t run_iteration(void)
{
	struct shared_ctl *ctl;
	pid_t pid;
	unsigned char *parent_base;
	bool *is_punched;
	size_t i;
	size_t child_failures, child_verified, parent_failures;
	size_t child_status_failures = 0;
	int status;
	size_t n_punched = 0;

	ctl = (struct shared_ctl *)mmap(NULL, sizeof(struct shared_ctl), PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ctl == MAP_FAILED)
		ksft_exit_fail_msg("mmap ctl failed: %d\n", get_errno());

	memset(ctl, 0, sizeof(struct shared_ctl));

	parent_base = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (parent_base == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %d\n", get_errno());

	pid = fork();
	if (pid < 0)
		ksft_exit_fail_msg("fork failed: %d\n", get_errno());

	if (pid == 0) {
		/* ---- Child process ---- */
		unsigned char *child_base = parent_base;

		/* in case parent exit abnormally, reader_thread() loop forever */
		if (prctl(PR_SET_PDEATHSIG, SIGTERM))
			ksft_exit_fail_msg("prctl(PR_SET_PDEATHSIG) failed: %d\n", get_errno());

		if (madvise(child_base, FILE_SIZE, MADV_HUGEPAGE) != 0)
			ksft_exit_fail_msg("madvise(MADV_HUGEPAGE) failed: %d\n",
				get_errno());

		for (i = 0; i < TOTAL_PAGES; i++)
			fill_page(child_base, i);

		if (!check_huge_shmem(child_base, NR_PMD_PAGE, pmd_pagesize)) {
			atomic_store_explicit(&ctl->ready, CHILD_FAILED, memory_order_release);
			ksft_print_msg("No shmem THP is allocated\n");
			_exit(0);
		}

		atomic_store_explicit(&ctl->ready, CHILD_READY, memory_order_release);
		child_reader_loop(child_base, ctl);

		munmap(child_base, FILE_SIZE);
		_exit(0);
	}

	/* ---- Parent process ---- */
	while (atomic_load_explicit(&ctl->ready, memory_order_acquire) == 0) {
		if (waitpid(pid, &status, WNOHANG) == pid) {
			ksft_print_msg(
				"Child terminated unexpectedly before ready\n");
			/* Force the ready flag to break loop and fail. */
			atomic_store_explicit(&ctl->ready, CHILD_FAILED,
					      memory_order_release);
			break;
		}
		usleep(1000);
	}

	if (ctl->ready == CHILD_FAILED)
		ksft_exit_fail_msg("Child process error\n");

	is_punched = calloc(TOTAL_PAGES, sizeof(bool));
	if (!is_punched)
		ksft_exit_fail_msg("calloc is_punched failed\n");

	for (i = 0; i < TOTAL_PAGES; i++) {
		int j;

		if (i % PUNCH_INTERVAL != 0)
			continue;
		if (madvise(parent_base + i * page_size,
			    PUNCH_SIZE_FACTOR * page_size, MADV_REMOVE) != 0) {
			ksft_exit_fail_msg(
				"madvise(MADV_REMOVE) failed on page %zu: %d\n",
				i, get_errno());
		}
		for (j = 0; j < PUNCH_SIZE_FACTOR && i + j < TOTAL_PAGES; j++)
			is_punched[i + j] = true;

		i += PUNCH_SIZE_FACTOR;

		n_punched += PUNCH_SIZE_FACTOR;
	}

	atomic_store_explicit(&ctl->stop, 1, memory_order_release);

	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_msg("waitpid failed\n");

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		child_status_failures = 1;
		if (WIFEXITED(status)) {
			ksft_print_msg("Child exited with non-zero status: %d\n",
				       WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			ksft_print_msg("Child terminated by signal: %s (%d)\n",
				       strsignal(WTERMSIG(status)),
				       WTERMSIG(status));
		} else {
			ksft_print_msg("Child exited with unknown status: 0x%x\n",
				       status);
		}
	}

	child_failures = atomic_load_explicit(&ctl->child_failures,
					      memory_order_acquire);
	child_verified = atomic_load_explicit(&ctl->child_verified,
					      memory_order_acquire);
	if (child_failures)
		ksft_print_msg("Child: %zu pages verified, %zu failures\n",
			child_verified, child_failures);

	parent_failures = verify_pages(parent_base, is_punched);
	if (parent_failures)
		ksft_print_msg("Parent verification failures: %zu\n",
			parent_failures);

	munmap(parent_base, FILE_SIZE);
	munmap(ctl, sizeof(struct shared_ctl));
	free(is_punched);

	(void)n_punched;
	return child_failures + parent_failures + child_status_failures;
}


static void thp_cleanup_handler(int signum)
{
	thp_restore_settings();
	/*
	 * Restore default handler and re-raise the signal to exit.
	 * This is to ensure the test process exits with the correct
	 * status code corresponding to the signal.
	 */
	signal(signum, SIG_DFL);
	raise(signum);
}

static void thp_settings_cleanup(void)
{
	thp_restore_settings();
}

int main(void)
{
	size_t iter;
	size_t failures;
	struct thp_settings current_settings;
	bool failed = false;

	ksft_print_header();

	if (!thp_is_enabled())
		ksft_exit_skip("Transparent Hugepages not available\n");

	if (geteuid() != 0)
		ksft_exit_skip("Please run the benchmark as root\n");

	thp_save_settings();
	/* make sure thp settings are restored */
	if (atexit(thp_settings_cleanup) != 0)
		ksft_exit_fail_msg("atexit failed\n");
	signal(SIGINT, thp_cleanup_handler);
	signal(SIGTERM, thp_cleanup_handler);

	thp_read_settings(&current_settings);
	current_settings.shmem_enabled = SHMEM_ADVISE;
	thp_write_settings(&current_settings);

	ksft_set_plan(1);

	page_size = getpagesize();
	pmd_pagesize = read_pmd_pagesize();

	ksft_print_msg("folio split race test\n");
	ksft_print_msg("=======================================================\n");
	ksft_print_msg("Shmem size:       %zu MiB\n", FILE_SIZE / 1024 / 1024);
	ksft_print_msg("Total pages:     %zu\n", TOTAL_PAGES);
	ksft_print_msg("Child readers:   %d\n", NUM_READER_THREADS);
	ksft_print_msg("Punching every %dth to %dth page\n", PUNCH_INTERVAL,
	       PUNCH_INTERVAL + PUNCH_SIZE_FACTOR);
	ksft_print_msg("Iterations:      %d\n", NUM_ITERATIONS);

	for (iter = 1; iter <= NUM_ITERATIONS; iter++) {
		failures = run_iteration();
		if (failures > 0) {
			failed = true;
			ksft_print_msg(
				"FAILED on iteration %zu: %zu pages corrupted by cross-process MADV_REMOVE!\n",
				iter, failures);
			break;
		}
	}

	if (failed) {
		ksft_test_result_fail("Test failed\n");
		ksft_exit_fail();
	} else {
		ksft_test_result_pass("All %d iterations passed\n", NUM_ITERATIONS);
		ksft_exit_pass();
	}

	return 0;
}
