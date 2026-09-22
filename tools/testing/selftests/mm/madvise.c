// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/mman.h>
#include <linux/mempolicy.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"
#include "vm_util.h"

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define NR_CONCURRENT_THREADS 8
#define NR_CONCURRENT_THPS 8
#define NR_CONCURRENT_ROUNDS 400
#define NR_MIGRATION_RACE_THPS 16
#define NR_MIGRATION_RACE_ROUNDS 20
#define NUMA_MASK_BITS 1024
#define NUMA_MASK_LONGS (NUMA_MASK_BITS / (8 * sizeof(unsigned long)))

static pthread_barrier_t concurrent_start_barrier;
static pthread_barrier_t concurrent_done_barrier;
static atomic_int concurrent_worker_errno;
static char *concurrent_area;
static size_t pmd_size;

#if defined(SYS_get_mempolicy) && defined(SYS_mbind) && \
	defined(SYS_migrate_pages)
struct migration_pageout_data {
	pthread_barrier_t start_barrier;
	atomic_bool stop;
	atomic_int calls;
	atomic_int error;
	char *mapping;
	size_t size;
};

static bool find_two_memory_nodes(unsigned long *mask, int *node1, int *node2)
{
	int node;

	if (syscall(SYS_get_mempolicy, NULL, mask, NUMA_MASK_BITS + 1, NULL,
		    MPOL_F_MEMS_ALLOWED))
		return false;

	*node1 = *node2 = -1;
	for (node = 0; node < NUMA_MASK_BITS; node++) {
		if (!(mask[node / (8 * sizeof(*mask))] &
		      (1UL << (node % (8 * sizeof(*mask))))))
			continue;
		if (*node1 < 0) {
			*node1 = node;
		} else {
			*node2 = node;
			return true;
		}
	}

	return false;
}

static void numa_mask_set(unsigned long *mask, int node)
{
	mask[node / (8 * sizeof(*mask))] |=
		1UL << (node % (8 * sizeof(*mask)));
}

static void *madvise_pageout_worker(void *arg)
{
	struct migration_pageout_data *data = arg;

	pthread_barrier_wait(&data->start_barrier);
	while (!atomic_load_explicit(&data->stop, memory_order_relaxed)) {
		if (madvise(data->mapping, data->size, MADV_PAGEOUT) &&
		    errno != EAGAIN) {
			atomic_store(&data->error, errno);
			break;
		}
		atomic_fetch_add(&data->calls, 1);
	}

	return NULL;
}

static int migrate_between_nodes(unsigned long *old_nodes,
				 unsigned long *new_nodes)
{
	int i;

	for (i = 0; i < NR_MIGRATION_RACE_ROUNDS; i++) {
		unsigned long *tmp;

		if (syscall(SYS_migrate_pages, 0, NUMA_MASK_BITS + 1,
			    old_nodes, new_nodes) < 0)
			return errno;
		tmp = old_nodes;
		old_nodes = new_nodes;
		new_nodes = tmp;
	}

	return 0;
}

static int race_pageout_with_migration(char *mapping, size_t size,
				       unsigned long *mask1,
				       unsigned long *mask2)
{
	struct migration_pageout_data data = {
		.mapping = mapping,
		.size = size,
	};
	pthread_t thread;
	int ret;

	ret = pthread_barrier_init(&data.start_barrier, NULL, 2);
	if (ret)
		return ret;
	ret = pthread_create(&thread, NULL, madvise_pageout_worker, &data);
	if (ret) {
		pthread_barrier_destroy(&data.start_barrier);
		return ret;
	}
	pthread_barrier_wait(&data.start_barrier);

	ret = migrate_between_nodes(mask1, mask2);
	atomic_store(&data.stop, true);
	pthread_join(thread, NULL);
	pthread_barrier_destroy(&data.start_barrier);

	if (ret)
		return ret;
	if (atomic_load(&data.error))
		return atomic_load(&data.error);
	return atomic_load(&data.calls) ? 0 : EIO;
}
#endif

static char *map_aligned_pages(size_t size)
{
	char *mapping, *aligned;

	mapping = mmap(NULL, size + pmd_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap");

	aligned = (char *)(((uintptr_t)mapping + pmd_size - 1) &
			   ~(pmd_size - 1));
	if (aligned != mapping)
		munmap(mapping, aligned - mapping);
	if (aligned + size < mapping + size + pmd_size)
		munmap(aligned + size,
		       mapping + size + pmd_size - (aligned + size));

	memset(aligned, 1, size);
	return aligned;
}

/* MADV_COLLAPSE may fail transiently with EAGAIN. */
static bool collapse_all(char *mapping, size_t size, int nr_hpages)
{
	int ret, retry;

	for (retry = 0; retry < 10; retry++) {
		ret = madvise(mapping, size, MADV_COLLAPSE);
		if (!ret) {
			if (check_huge_anon(mapping, size, nr_hpages, pmd_size))
				return true;
		} else if (errno != EAGAIN) {
			return false;
		}
		usleep(10000);
	}

	return false;
}

static bool collapse_shmem(char *mapping, size_t size, int nr_hpages)
{
	int ret, retry;

	for (retry = 0; retry < 10; retry++) {
		ret = madvise(mapping, size, MADV_COLLAPSE);
		if (!ret) {
			if (check_huge_shmem(mapping, size, nr_hpages, pmd_size))
				return true;
		} else if (errno != EAGAIN) {
			return false;
		}
		usleep(10000);
	}

	return false;
}

static char *map_unevictable_shmem(size_t size, int *shmid)
{
	char *reservation, *mapping, *aligned;

	reservation = mmap(NULL, size + pmd_size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_perror("mmap reservation");
	aligned = (char *)(((uintptr_t)reservation + pmd_size - 1) &
			   ~(pmd_size - 1));
	munmap(reservation, size + pmd_size);

	*shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
	if (*shmid < 0)
		ksft_exit_fail_perror("shmget");
	if (shmctl(*shmid, SHM_LOCK, NULL)) {
		shmctl(*shmid, IPC_RMID, NULL);
		ksft_test_result_skip("could not lock a shmem segment\n");
		return MAP_FAILED;
	}
	mapping = shmat(*shmid, aligned, 0);
	if (mapping == (void *)-1)
		ksft_exit_fail_perror("shmat");
	if (mapping != aligned)
		ksft_exit_fail_msg("shmat did not honor the aligned address\n");

	return mapping;
}

static void unmap_unevictable_shmem(char *mapping, int shmid)
{
	shmctl(shmid, SHM_UNLOCK, NULL);
	shmdt(mapping);
	shmctl(shmid, IPC_RMID, NULL);
}

static char *map_aligned_file(int fd, size_t size, int flags)
{
	char *mapping, *aligned;

	mapping = mmap(NULL, size + pmd_size, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap reservation");
	aligned = (char *)(((uintptr_t)mapping + pmd_size - 1) &
			   ~(pmd_size - 1));
	if (mmap(aligned, size, PROT_READ | PROT_WRITE, flags | MAP_FIXED,
		 fd, 0) == MAP_FAILED)
		ksft_exit_fail_perror("mmap file");
	if (aligned != mapping)
		munmap(mapping, aligned - mapping);
	if (aligned + size < mapping + size + pmd_size)
		munmap(aligned + size,
		       mapping + size + pmd_size - (aligned + size));

	return aligned;
}

static void split_pmd_mapping(char *mapping)
{
	const size_t page_size = getpagesize();

	if (mprotect(mapping + page_size, page_size, PROT_READ) ||
	    mprotect(mapping + page_size, page_size, PROT_READ | PROT_WRITE))
		ksft_exit_fail_perror("mprotect");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("mprotect split the physical THP\n");
}

static void check_memory(char *mapping, size_t size)
{
	size_t offset;

	for (offset = 0; offset < size; offset += getpagesize())
		if (mapping[offset] != 1)
			ksft_exit_fail_msg("memory changed at offset %zu\n", offset);
}

/*
 * MADV_COLD on a full PTE-mapped THP must preserve the folio, while an
 * operation on only half of it must split the folio. Neither operation may
 * alter the mapping contents.
 */
static void test_pte_mapped_madvise_cold(void)
{
	char *mapping = map_aligned_pages(pmd_size);

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}

	split_pmd_mapping(mapping);
	if (madvise(mapping, pmd_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("full MADV_COLD split a large folio\n");

	if (madvise(mapping, pmd_size / 2, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 0, pmd_size))
		ksft_exit_fail_msg("partial MADV_COLD left a large folio\n");
	check_memory(mapping, pmd_size);

	munmap(mapping, pmd_size);
	ksft_test_result_pass("MADV_COLD handles a PTE-mapped THP\n");
}

/*
 * A PTE walk must skip holes without populating them or overlooking the
 * present pages on either side.
 */
static void test_madvise_cold_pte_hole(void)
{
	const size_t page_size = getpagesize();
	const size_t size = 3 * page_size;
	char *mapping;
	int pagemap_fd;

	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	mapping[0] = 1;
	mapping[2 * page_size] = 1;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open pagemap");
	if (pagemap_is_populated(pagemap_fd, mapping + page_size))
		ksft_exit_fail_msg("PTE hole was populated before MADV_COLD\n");

	if (madvise(mapping, size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (mapping[0] != 1 || mapping[2 * page_size] != 1)
		ksft_exit_fail_msg("MADV_COLD changed populated pages\n");
	if (pagemap_is_populated(pagemap_fd, mapping + page_size))
		ksft_exit_fail_msg("MADV_COLD populated a PTE hole\n");

	close(pagemap_fd);
	munmap(mapping, size);
	ksft_test_result_pass("MADV_COLD skips PTE holes\n");
}

/*
 * A read fault on private anonymous memory may install the shared zero page.
 * MADV_COLD must ignore that special PTE without replacing it or changing the
 * mapping contents.
 */
static void test_madvise_cold_zero_page(void)
{
	const size_t page_size = getpagesize();
	char *mapping;
	int pagemap_fd;

	mapping = mmap(NULL, page_size, PROT_READ,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	if (mapping[0])
		ksft_exit_fail_msg("anonymous mapping is not zero-filled\n");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open pagemap");
	if (!pagemap_is_populated(pagemap_fd, mapping))
		ksft_exit_fail_msg("zero page is not populated\n");

	if (madvise(mapping, page_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (mapping[0] || !pagemap_is_populated(pagemap_fd, mapping))
		ksft_exit_fail_msg("MADV_COLD changed the zero-page mapping\n");

	close(pagemap_fd);
	munmap(mapping, page_size);
	ksft_test_result_pass("MADV_COLD skips the shared zero page\n");
}

/*
 * A read fault may map the shared huge zero page with a PMD. MADV_COLD must
 * ignore that special PMD without replacing it or changing the mapping.
 */
static void test_madvise_cold_huge_zero_page(void)
{
	char *reservation, *mapping;
	unsigned long pfn;
	uint64_t flags;
	int pagemap_fd, kpageflags_fd;

	if (geteuid()) {
		ksft_test_result_skip("requires root to read page flags\n");
		return;
	}
	reservation = mmap(NULL, 2 * pmd_size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_perror("mmap reservation");
	mapping = (char *)(((uintptr_t)reservation + pmd_size - 1) &
			   ~(pmd_size - 1));
	if (mmap(mapping, pmd_size, PROT_READ,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
		ksft_exit_fail_perror("mmap huge zero page");
	if (mapping != reservation)
		munmap(reservation, mapping - reservation);
	if (mapping + pmd_size < reservation + 2 * pmd_size)
		munmap(mapping + pmd_size,
		       reservation + 2 * pmd_size - (mapping + pmd_size));
	if (madvise(mapping, pmd_size, MADV_HUGEPAGE))
		ksft_exit_fail_perror("MADV_HUGEPAGE");
	if (mapping[0])
		ksft_exit_fail_msg("anonymous mapping is not zero-filled\n");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
	if (pagemap_fd < 0 || kpageflags_fd < 0)
		ksft_exit_fail_perror("open page flags");
	pfn = pagemap_get_pfn(pagemap_fd, mapping);
	if (pfn == -1ul || pageflags_get(pfn, kpageflags_fd, &flags) ||
	    !(flags & KPF_ZERO_PAGE)) {
		close(kpageflags_fd);
		close(pagemap_fd);
		munmap(mapping, pmd_size);
		ksft_test_result_skip("huge zero page is not available\n");
		return;
	}

	if (madvise(mapping, pmd_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (mapping[0] || pagemap_get_pfn(pagemap_fd, mapping) != pfn)
		ksft_exit_fail_msg("MADV_COLD changed the huge-zero-page mapping\n");

	close(kpageflags_fd);
	close(pagemap_fd);
	munmap(mapping, pmd_size);
	ksft_test_result_pass("MADV_COLD skips the huge zero page\n");
}

/*
 * Neither hint is valid for a locked VMA: reclaiming it would violate the
 * mlock contract, and merely aging it would serve no purpose.
 */
static void test_madvise_lru_locked_vma(void)
{
	const size_t page_size = getpagesize();
	char *mapping;

	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	mapping[0] = 1;
	if (mlock(mapping, page_size)) {
		munmap(mapping, page_size);
		ksft_test_result_skip("could not lock a page\n");
		return;
	}

	errno = 0;
	if (!madvise(mapping, page_size, MADV_COLD) || errno != EINVAL)
		ksft_exit_fail_msg("MADV_COLD accepted a locked VMA\n");
	errno = 0;
	if (!madvise(mapping, page_size, MADV_PAGEOUT) || errno != EINVAL)
		ksft_exit_fail_msg("MADV_PAGEOUT accepted a locked VMA\n");
	if (mapping[0] != 1)
		ksft_exit_fail_msg("madvise changed locked memory\n");

	munlock(mapping, page_size);
	munmap(mapping, page_size);
	ksft_test_result_pass("COLD and PAGEOUT reject a locked VMA\n");
}

/*
 * A full-range MADV_COLD operates directly on a huge PMD. Assert that aging
 * the mapping preserves both the physical THP and its contents.
 */
static void test_full_pmd_madvise_cold(void)
{
	char *mapping = map_aligned_pages(pmd_size);

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	if (madvise(mapping, pmd_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("full MADV_COLD split a PMD-mapped THP\n");
	check_memory(mapping, pmd_size);

	munmap(mapping, pmd_size);
	ksft_test_result_pass("full MADV_COLD preserves a PMD-mapped THP\n");
}

/*
 * MADV_COLD is idempotent. A second request must handle an already-old huge
 * PMD without splitting the THP or changing its contents.
 */
static void test_repeated_pmd_madvise_cold(void)
{
	char *mapping = map_aligned_pages(pmd_size);

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	if (madvise(mapping, pmd_size, MADV_COLD) ||
	    madvise(mapping, pmd_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("repeated MADV_COLD split a PMD-mapped THP\n");
	check_memory(mapping, pmd_size);

	munmap(mapping, pmd_size);
	ksft_test_result_pass("repeated MADV_COLD preserves a PMD-mapped THP\n");
}

/*
 * MADV_COLD on half of a PMD-mapped THP must split the folio so the
 * unadvised half is not aged as part of the THP.
 */
static void test_partial_pmd_madvise_cold(void)
{
	char *mapping = map_aligned_pages(pmd_size);

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	if (madvise(mapping, pmd_size / 2, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 0, pmd_size))
		ksft_exit_fail_msg("partial MADV_COLD left a PMD-mapped THP\n");
	check_memory(mapping, pmd_size);

	munmap(mapping, pmd_size);
	ksft_test_result_pass("partial MADV_COLD splits a PMD-mapped THP\n");
}

static void pageout_half_thp(char *mapping)
{
	if (madvise(mapping, pmd_size / 2, MADV_PAGEOUT))
		ksft_exit_fail_perror("MADV_PAGEOUT");
	if (!check_large_folios(mapping, pmd_size, 0, pmd_size))
		ksft_exit_fail_msg("partial MADV_PAGEOUT left a large folio\n");
	check_memory(mapping, pmd_size);
}

/*
 * Full-range MADV_PAGEOUT can reclaim a PMD-mapped THP directly. Assert that
 * the mapping is swapped without corrupting the folio contents.
 */
static void test_full_pmd_madvise_pageout(void)
{
	char *mapping = map_aligned_pages(pmd_size);
	int pagemap_fd, retry;
	bool swapped = false;

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open pagemap");

	for (retry = 0; retry < 100; retry++) {
		if (madvise(mapping, pmd_size, MADV_PAGEOUT))
			ksft_exit_fail_perror("MADV_PAGEOUT");
		if (pagemap_is_swapped(pagemap_fd, mapping)) {
			swapped = true;
			break;
		}
		usleep(10000);
	}
	if (swapped)
		check_memory(mapping, pmd_size);

	close(pagemap_fd);
	munmap(mapping, pmd_size);
	if (!swapped) {
		ksft_test_result_skip("MADV_PAGEOUT did not swap the THP\n");
		return;
	}
	ksft_test_result_pass("full MADV_PAGEOUT swaps a PMD-mapped THP\n");
}

/*
 * A swapped PTE is non-present but not empty. MADV_COLD must skip the entry
 * without faulting the page back in or changing its contents.
 */
static void test_madvise_cold_swapped_pte(void)
{
	const size_t page_size = getpagesize();
	char *mapping = map_aligned_pages(page_size);
	int pagemap_fd, retry;
	bool swapped = false;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open pagemap");
	for (retry = 0; retry < 100; retry++) {
		if (madvise(mapping, page_size, MADV_PAGEOUT))
			ksft_exit_fail_perror("MADV_PAGEOUT");
		if (pagemap_is_swapped(pagemap_fd, mapping)) {
			swapped = true;
			break;
		}
		usleep(10000);
	}
	if (!swapped) {
		close(pagemap_fd);
		munmap(mapping, page_size);
		ksft_test_result_skip("MADV_PAGEOUT did not swap the page\n");
		return;
	}

	if (madvise(mapping, page_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!pagemap_is_swapped(pagemap_fd, mapping))
		ksft_exit_fail_msg("MADV_COLD faulted in a swapped PTE\n");
	if (mapping[0] != 1)
		ksft_exit_fail_msg("swapped page contents changed\n");

	close(pagemap_fd);
	munmap(mapping, page_size);
	ksft_test_result_pass("MADV_COLD skips a swapped PTE\n");
}

/*
 * vmsplice() retains a reference to a THP in a pipe, preventing a partial
 * MADV_COLD from splitting it. The advice must leave the folio intact while
 * pinned, then split it normally after the pipe releases the reference.
 */
static void test_partial_pinned_madvise_cold(bool pte_mapped)
{
	const size_t page_size = getpagesize();
	char *mapping = map_aligned_pages(pmd_size);
	struct iovec iov = {
		.iov_base = mapping,
		.iov_len = page_size,
	};
	char *buffer;
	int pipefd[2];
	int retry;

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	if (pte_mapped)
		split_pmd_mapping(mapping);
	if (pipe(pipefd))
		ksft_exit_fail_perror("pipe");
	if (vmsplice(pipefd[1], &iov, 1, SPLICE_F_GIFT) != page_size) {
		close(pipefd[0]);
		close(pipefd[1]);
		munmap(mapping, pmd_size);
		ksft_test_result_skip("vmsplice could not retain a THP page\n");
		return;
	}

	if (madvise(mapping, pmd_size / 2, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("MADV_COLD split a pinned THP\n");

	buffer = malloc(page_size);
	if (!buffer)
		ksft_exit_fail_perror("malloc");
	if (read(pipefd[0], buffer, page_size) != page_size)
		ksft_exit_fail_perror("read pipe");
	free(buffer);
	close(pipefd[0]);
	close(pipefd[1]);

	for (retry = 0; retry < 10; retry++) {
		if (madvise(mapping, pmd_size / 2, MADV_COLD))
			ksft_exit_fail_perror("MADV_COLD");
		if (check_large_folios(mapping, pmd_size, 0, pmd_size))
			break;
		usleep(10000);
	}
	if (retry == 10)
		ksft_exit_fail_msg("MADV_COLD did not split an unpinned THP\n");
	check_memory(mapping, pmd_size);

	munmap(mapping, pmd_size);
	ksft_test_result_pass("partial MADV_COLD skips a pinned %s THP\n",
			      pte_mapped ? "PTE-mapped" : "PMD-mapped");
}

static void test_partial_pinned_pmd_madvise_cold(void)
{
	test_partial_pinned_madvise_cold(false);
}

/*
 * Pinning a PTE-mapped THP also prevents a partial MADV_COLD from splitting
 * it. Once the pipe releases the reference, a serial retry must split it.
 */
static void test_partial_pinned_pte_madvise_cold(void)
{
	test_partial_pinned_madvise_cold(true);
}

static void *madvise_cold_worker(void *unused)
{
	int round, i;

	(void)unused;
	for (round = 0; round < NR_CONCURRENT_ROUNDS; round++) {
		pthread_barrier_wait(&concurrent_start_barrier);
		for (i = 0; i < NR_CONCURRENT_THPS; i++) {
			if (madvise(concurrent_area + i * pmd_size,
				    pmd_size / 2, MADV_COLD))
				atomic_store(&concurrent_worker_errno, errno);
		}
		pthread_barrier_wait(&concurrent_done_barrier);
	}

	return NULL;
}

/*
 * Concurrent partial MADV_COLD calls deliberately contend for each THP lock.
 * Alternate PMD- and PTE-mapped rounds to exercise both split paths. A caller
 * that loses the trylock race may skip that folio, so retry serially before
 * asserting the stable interface: all calls succeed, the THPs remain
 * splittable, and their contents are unchanged.
 */
static void test_concurrent_partial_madvise_cold(void)
{
	const size_t size = NR_CONCURRENT_THPS * pmd_size;
	pthread_t threads[NR_CONCURRENT_THREADS];
	int round, i;

	concurrent_area = map_aligned_pages(size);
	if (!collapse_all(concurrent_area, size, NR_CONCURRENT_THPS)) {
		munmap(concurrent_area, size);
		ksft_test_result_skip("could not allocate PMD-sized THPs\n");
		return;
	}
	if (pthread_barrier_init(&concurrent_start_barrier, NULL,
				 NR_CONCURRENT_THREADS + 1) ||
	    pthread_barrier_init(&concurrent_done_barrier, NULL,
				 NR_CONCURRENT_THREADS + 1))
		ksft_exit_fail_msg("pthread_barrier_init failed\n");
	for (i = 0; i < NR_CONCURRENT_THREADS; i++)
		if (pthread_create(&threads[i], NULL, madvise_cold_worker, NULL))
			ksft_exit_fail_msg("pthread_create failed\n");

	for (round = 0; round < NR_CONCURRENT_ROUNDS; round++) {
		if (!collapse_all(concurrent_area, size, NR_CONCURRENT_THPS))
			ksft_exit_fail_msg("round %d: failed to form PMD THPs\n",
					   round);
		if (round & 1)
			for (i = 0; i < NR_CONCURRENT_THPS; i++)
				split_pmd_mapping(concurrent_area + i * pmd_size);
		pthread_barrier_wait(&concurrent_start_barrier);
		pthread_barrier_wait(&concurrent_done_barrier);

		if (atomic_load(&concurrent_worker_errno)) {
			errno = atomic_load(&concurrent_worker_errno);
			ksft_exit_fail_perror("MADV_COLD");
		}
		for (i = 0; i < NR_CONCURRENT_THPS; i++) {
			if (madvise(concurrent_area + i * pmd_size,
				    pmd_size / 2, MADV_COLD))
				ksft_exit_fail_perror("MADV_COLD retry");
		}
		if (!check_large_folios(concurrent_area, size, 0, pmd_size))
			ksft_exit_fail_msg("round %d: PMD THP remained\n", round);
	}
	check_memory(concurrent_area, size);

	for (i = 0; i < NR_CONCURRENT_THREADS; i++)
		pthread_join(threads[i], NULL);
	pthread_barrier_destroy(&concurrent_done_barrier);
	pthread_barrier_destroy(&concurrent_start_barrier);
	munmap(concurrent_area, size);
	ksft_test_result_pass("concurrent partial MADV_COLD preserves memory\n");
}

/*
 * SHM_LOCK makes a shmem folio unevictable without setting VM_LOCKED on this
 * VMA. MADV_PAGEOUT must put an isolated folio back on its LRU rather than
 * reclaiming it.
 */
static void test_madvise_pageout_unevictable(void)
{
	const size_t page_size = getpagesize();
	unsigned long pfn;
	uint64_t flags;
	char *mapping;
	int pagemap_fd, kpageflags_fd;
	int shmid;

	if (geteuid()) {
		ksft_test_result_skip("requires root to read page flags\n");
		return;
	}
	shmid = shmget(IPC_PRIVATE, page_size, IPC_CREAT | 0600);
	if (shmid < 0)
		ksft_exit_fail_perror("shmget");
	if (shmctl(shmid, SHM_LOCK, NULL)) {
		shmctl(shmid, IPC_RMID, NULL);
		ksft_test_result_skip("could not lock a shmem segment\n");
		return;
	}
	mapping = shmat(shmid, NULL, 0);
	if (mapping == (void *)-1)
		ksft_exit_fail_perror("shmat");
	mapping[0] = 1;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
	if (pagemap_fd < 0 || kpageflags_fd < 0)
		ksft_exit_fail_perror("open page flags");

	/* Drain the LRU add batch so the locked mapping becomes unevictable. */
	if (madvise(mapping, page_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	pfn = pagemap_get_pfn(pagemap_fd, mapping);
	if (pfn == -1ul || pageflags_get(pfn, kpageflags_fd, &flags) ||
	    !(flags & KPF_UNEVICTABLE))
		ksft_exit_fail_msg("SHM_LOCK page is not unevictable\n");

	if (madvise(mapping, page_size, MADV_PAGEOUT))
		ksft_exit_fail_perror("MADV_PAGEOUT");
	if (!pagemap_is_populated(pagemap_fd, mapping) || mapping[0] != 1)
		ksft_exit_fail_msg("MADV_PAGEOUT reclaimed an unevictable page\n");

	close(kpageflags_fd);
	close(pagemap_fd);
	shmctl(shmid, SHM_UNLOCK, NULL);
	shmdt(mapping);
	shmctl(shmid, IPC_RMID, NULL);
	ksft_test_result_pass("MADV_PAGEOUT preserves an unevictable page\n");
}

/*
 * Exercise the unevictable PAGEOUT path at PMD granularity. SHM_LOCK keeps
 * the shmem THP resident without marking its VMA VM_LOCKED.
 */
static void test_madvise_pageout_unevictable_thp(void)
{
	char *mapping;
	int pagemap_fd, shmid;

	if (geteuid()) {
		ksft_test_result_skip("requires root to lock a shmem segment\n");
		return;
	}
	mapping = map_unevictable_shmem(pmd_size, &shmid);
	if (mapping == MAP_FAILED)
		return;
	memset(mapping, 1, pmd_size);
	if (!collapse_shmem(mapping, pmd_size, 1)) {
		unmap_unevictable_shmem(mapping, shmid);
		ksft_test_result_skip("could not allocate an unevictable THP\n");
		return;
	}
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open pagemap");

	if (madvise(mapping, pmd_size, MADV_PAGEOUT))
		ksft_exit_fail_perror("MADV_PAGEOUT");
	if (!pagemap_is_populated(pagemap_fd, mapping) ||
	    !check_huge_shmem(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("MADV_PAGEOUT reclaimed an unevictable THP\n");
	check_memory(mapping, pmd_size);

	close(pagemap_fd);
	unmap_unevictable_shmem(mapping, shmid);
	ksft_test_result_pass("MADV_PAGEOUT preserves an unevictable THP\n");
}

/*
 * migrate_pages() isolates folios from the LRU before replacing their page
 * table entries. Race that interval against MADV_PAGEOUT on PMD-mapped THPs.
 * Either operation may skip a folio owned by the other, but repeated calls
 * must not report an error or corrupt the mapping.
 */
static void test_madvise_pageout_migration(void)
{
#if defined(SYS_get_mempolicy) && defined(SYS_mbind) && \
	defined(SYS_migrate_pages)
	unsigned long allowed[NUMA_MASK_LONGS] = {};
	unsigned long mask1[NUMA_MASK_LONGS] = {};
	unsigned long mask2[NUMA_MASK_LONGS] = {};
	const size_t size = NR_MIGRATION_RACE_THPS * pmd_size;
	char *mapping;
	int node1, node2;
	int error;
	int shmid;

	if (geteuid()) {
		ksft_test_result_skip("requires root to lock a shmem segment\n");
		return;
	}
	if (!find_two_memory_nodes(allowed, &node1, &node2)) {
		ksft_test_result_skip("requires two allowed NUMA memory nodes\n");
		return;
	}

	numa_mask_set(mask1, node1);
	numa_mask_set(mask2, node2);
	mapping = map_unevictable_shmem(size, &shmid);
	if (mapping == MAP_FAILED)
		return;
	if (syscall(SYS_mbind, mapping, size, MPOL_BIND, mask1,
		    NUMA_MASK_BITS + 1, 0)) {
		unmap_unevictable_shmem(mapping, shmid);
		ksft_test_result_skip("could not bind shmem to a NUMA node\n");
		return;
	}
	memset(mapping, 1, size);
	if (!collapse_shmem(mapping, size, NR_MIGRATION_RACE_THPS)) {
		unmap_unevictable_shmem(mapping, shmid);
		ksft_test_result_skip("could not allocate unevictable shmem THPs\n");
		return;
	}

	error = race_pageout_with_migration(mapping, size, mask1, mask2);
	if (error == ENOSYS) {
		unmap_unevictable_shmem(mapping, shmid);
		ksft_test_result_skip("NUMA migration is unavailable\n");
		return;
	}
	if (error) {
		errno = error;
		ksft_exit_fail_perror("MADV_PAGEOUT/migrate_pages race");
	}
	check_memory(mapping, size);

	unmap_unevictable_shmem(mapping, shmid);
	ksft_test_result_pass("MADV_PAGEOUT races NUMA migration safely\n");
#else
	ksft_test_result_skip("NUMA migration system calls are unavailable\n");
#endif
}

static bool activate_folio(char *mapping, char *drain, unsigned long pfn,
			   int kpageflags_fd)
{
	struct iovec local = { .iov_base = drain, .iov_len = 1 };
	struct iovec remote = { .iov_base = mapping, .iov_len = 1 };
	uint64_t flags;
	int retry;

	for (retry = 0; retry < 10; retry++) {
		if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 1)
			ksft_exit_fail_perror("process_vm_readv");
		/* Drain the activation batch without advising the target folio. */
		if (madvise(drain, getpagesize(), MADV_COLD))
			ksft_exit_fail_perror("MADV_COLD");
		if (pageflags_get(pfn, kpageflags_fd, &flags))
			ksft_exit_fail_perror("read kpageflags");
		if (flags & KPF_ACTIVE)
			return true;
	}

	return false;
}

/*
 * Repeated GUP accesses promote an inactive folio on the traditional LRU.
 * MADV_COLD must deactivate that folio while preserving its contents.
 */
static void test_madvise_cold_active_folio(void)
{
	const size_t page_size = getpagesize();
	char *mapping = map_aligned_pages(page_size);
	char *drain = map_aligned_pages(page_size);
	cpu_set_t old_mask, mask;
	int pagemap_fd, kpageflags_fd;
	unsigned long pfn;
	uint64_t flags;
	int cpu;

	if (geteuid()) {
		munmap(drain, page_size);
		munmap(mapping, page_size);
		ksft_test_result_skip("requires root to read page flags\n");
		return;
	}
	if (sched_getaffinity(0, sizeof(old_mask), &old_mask))
		ksft_exit_fail_perror("sched_getaffinity");
	cpu = sched_getcpu();
	if (cpu < 0)
		ksft_exit_fail_perror("sched_getcpu");
	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask))
		ksft_exit_fail_perror("sched_setaffinity");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
	if (pagemap_fd < 0 || kpageflags_fd < 0)
		ksft_exit_fail_perror("open page flags");
	pfn = pagemap_get_pfn(pagemap_fd, mapping);
	if (pfn == -1ul)
		ksft_exit_fail_msg("could not read page PFN\n");

	if (!activate_folio(mapping, drain, pfn, kpageflags_fd)) {
		close(kpageflags_fd);
		close(pagemap_fd);
		sched_setaffinity(0, sizeof(old_mask), &old_mask);
		munmap(drain, page_size);
		munmap(mapping, page_size);
		ksft_test_result_skip("could not activate a folio\n");
		return;
	}

	if (madvise(mapping, page_size, MADV_COLD) ||
	    madvise(drain, page_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (pageflags_get(pfn, kpageflags_fd, &flags))
		ksft_exit_fail_perror("read kpageflags");
	if ((flags & KPF_ACTIVE) || mapping[0] != 1)
		ksft_exit_fail_msg("MADV_COLD did not deactivate the folio\n");

	close(kpageflags_fd);
	close(pagemap_fd);
	if (sched_setaffinity(0, sizeof(old_mask), &old_mask))
		ksft_exit_fail_perror("restore affinity");
	munmap(drain, page_size);
	munmap(mapping, page_size);
	ksft_test_result_pass("MADV_COLD deactivates an active folio\n");
}

struct remote_fault_data {
	char *mapping;
	pthread_barrier_t barrier;
	int cpu;
	int error;
};

static void *remote_fault_worker(void *arg)
{
	struct remote_fault_data *data = arg;
	cpu_set_t mask;

	CPU_ZERO(&mask);
	CPU_SET(data->cpu, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask))
		data->error = errno;
	else
		data->mapping[0] = 1;
	pthread_barrier_wait(&data->barrier);
	pthread_barrier_wait(&data->barrier);
	return NULL;
}

/*
 * A folio faulted on another CPU can remain in that CPU's pending LRU batch.
 * MADV_COLD drains only the calling CPU and must safely skip the non-LRU
 * folio without changing the mapping.
 */
static void test_madvise_cold_remote_lru_batch(void)
{
	const size_t page_size = getpagesize();
	struct remote_fault_data data = { .cpu = -1 };
	char *mapping;
	cpu_set_t old_mask, mask;
	pthread_t thread;
	unsigned long pfn;
	uint64_t flags;
	int pagemap_fd, kpageflags_fd;
	int cpu, main_cpu = -1;

	if (geteuid()) {
		ksft_test_result_skip("requires root to read page flags\n");
		return;
	}
	if (sched_getaffinity(0, sizeof(old_mask), &old_mask))
		ksft_exit_fail_perror("sched_getaffinity");
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &old_mask))
			continue;
		if (main_cpu < 0) {
			main_cpu = cpu;
		} else {
			data.cpu = cpu;
			break;
		}
	}
	if (main_cpu < 0 || data.cpu < 0) {
		ksft_test_result_skip("requires two CPUs\n");
		return;
	}
	CPU_ZERO(&mask);
	CPU_SET(main_cpu, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask))
		ksft_exit_fail_perror("sched_setaffinity");

	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	data.mapping = mapping;
	if (pthread_barrier_init(&data.barrier, NULL, 2) ||
	    pthread_create(&thread, NULL, remote_fault_worker, &data))
		ksft_exit_fail_msg("could not start fault worker\n");
	pthread_barrier_wait(&data.barrier);
	if (data.error) {
		errno = data.error;
		ksft_exit_fail_perror("worker sched_setaffinity");
	}

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
	if (pagemap_fd < 0 || kpageflags_fd < 0)
		ksft_exit_fail_perror("open page flags");
	pfn = pagemap_get_pfn(pagemap_fd, mapping);
	if (pfn == -1ul || pageflags_get(pfn, kpageflags_fd, &flags))
		ksft_exit_fail_msg("could not read page flags\n");
	if (flags & KPF_LRU) {
		pthread_barrier_wait(&data.barrier);
		pthread_join(thread, NULL);
		pthread_barrier_destroy(&data.barrier);
		close(kpageflags_fd);
		close(pagemap_fd);
		sched_setaffinity(0, sizeof(old_mask), &old_mask);
		munmap(mapping, page_size);
		ksft_test_result_skip("remote LRU batch was already drained\n");
		return;
	}

	if (madvise(mapping, page_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (mapping[0] != 1 || !pagemap_is_populated(pagemap_fd, mapping))
		ksft_exit_fail_msg("MADV_COLD changed a pending-LRU page\n");

	pthread_barrier_wait(&data.barrier);
	pthread_join(thread, NULL);
	pthread_barrier_destroy(&data.barrier);
	close(kpageflags_fd);
	close(pagemap_fd);
	if (sched_setaffinity(0, sizeof(old_mask), &old_mask))
		ksft_exit_fail_perror("restore affinity");
	munmap(mapping, page_size);
	ksft_test_result_pass("MADV_COLD skips a remote pending-LRU page\n");
}

/*
 * Exercise the same active-folio transition through a huge PMD, where the
 * page-table aging operation differs from the PTE implementation.
 */
static void test_pmd_madvise_cold_active_folio(void)
{
	const size_t page_size = getpagesize();
	char *mapping;
	char *drain = map_aligned_pages(page_size);
	cpu_set_t old_mask, mask;
	int pagemap_fd, kpageflags_fd;
	unsigned long pfn;
	uint64_t flags;
	int cpu, fd, retry;
	bool active = false;

	if (geteuid()) {
		munmap(drain, page_size);
		ksft_test_result_skip("requires root to read page flags\n");
		return;
	}
	fd = memfd_create("madvise-active-thp", 0);
	if (fd < 0)
		ksft_exit_fail_perror("memfd_create");
	if (ftruncate(fd, pmd_size))
		ksft_exit_fail_perror("ftruncate");
	mapping = map_aligned_file(fd, pmd_size, MAP_SHARED);
	memset(mapping, 1, pmd_size);
	if (msync(mapping, pmd_size, MS_SYNC) ||
	    !collapse_shmem(mapping, pmd_size, 1)) {
		munmap(drain, page_size);
		munmap(mapping, pmd_size);
		close(fd);
		ksft_test_result_skip("could not allocate a PMD-sized shmem THP\n");
		return;
	}
	if (sched_getaffinity(0, sizeof(old_mask), &old_mask))
		ksft_exit_fail_perror("sched_getaffinity");
	cpu = sched_getcpu();
	if (cpu < 0)
		ksft_exit_fail_perror("sched_getcpu");
	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask))
		ksft_exit_fail_perror("sched_setaffinity");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
	if (pagemap_fd < 0 || kpageflags_fd < 0)
		ksft_exit_fail_perror("open page flags");
	pfn = pagemap_get_pfn(pagemap_fd, mapping);
	if (pfn == -1ul)
		ksft_exit_fail_msg("could not read THP PFN\n");

	for (retry = 0; retry < 10; retry++) {
		if (pread(fd, drain, 1, 0) != 1)
			ksft_exit_fail_perror("pread");
		if (madvise(drain, page_size, MADV_COLD))
			ksft_exit_fail_perror("MADV_COLD");
		if (pageflags_get(pfn, kpageflags_fd, &flags))
			ksft_exit_fail_perror("read kpageflags");
		if (flags & KPF_ACTIVE) {
			active = true;
			break;
		}
	}
	if (!active) {
		close(kpageflags_fd);
		close(pagemap_fd);
		sched_setaffinity(0, sizeof(old_mask), &old_mask);
		munmap(drain, page_size);
		munmap(mapping, pmd_size);
		close(fd);
		ksft_test_result_skip("could not activate a THP\n");
		return;
	}

	if (madvise(mapping, pmd_size, MADV_COLD) ||
	    madvise(drain, page_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (pageflags_get(pfn, kpageflags_fd, &flags))
		ksft_exit_fail_perror("read kpageflags");
	if (flags & KPF_ACTIVE)
		ksft_exit_fail_msg("MADV_COLD did not deactivate the THP\n");
	if (!check_huge_shmem(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("MADV_COLD split an active THP\n");
	check_memory(mapping, pmd_size);

	close(kpageflags_fd);
	close(pagemap_fd);
	if (sched_setaffinity(0, sizeof(old_mask), &old_mask))
		ksft_exit_fail_perror("restore affinity");
	munmap(drain, page_size);
	munmap(mapping, pmd_size);
	close(fd);
	ksft_test_result_pass("MADV_COLD deactivates an active THP\n");
}

/*
 * MADV_PAGEOUT on half of a PMD-mapped THP must split the folio before
 * reclaim so the unadvised half is not reclaimed as part of the THP.
 */
static void test_partial_pmd_madvise_pageout(void)
{
	char *mapping = map_aligned_pages(pmd_size);

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	pageout_half_thp(mapping);

	munmap(mapping, pmd_size);
	ksft_test_result_pass("partial MADV_PAGEOUT splits a PMD-mapped THP\n");
}

/*
 * mprotect() replaces the huge PMD with PTEs without splitting the physical
 * THP. MADV_PAGEOUT on half of that PTE-mapped THP must split the folio.
 */
static void test_partial_pte_madvise_pageout(void)
{
	char *mapping = map_aligned_pages(pmd_size);

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	split_pmd_mapping(mapping);
	pageout_half_thp(mapping);

	munmap(mapping, pmd_size);
	ksft_test_result_pass("partial MADV_PAGEOUT splits a PTE-mapped THP\n");
}

static pid_t fork_waiting_child(int pipefd[2])
{
	pid_t pid;

	if (pipe(pipefd))
		ksft_exit_fail_perror("pipe");
	pid = fork();
	if (pid < 0)
		ksft_exit_fail_perror("fork");
	if (!pid) {
		char byte;

		close(pipefd[1]);
		while (read(pipefd[0], &byte, 1) < 0 && errno == EINTR)
			;
		_exit(0);
	}
	close(pipefd[0]);
	return pid;
}

/*
 * fork() gives the THP another mapping, then mprotect() PTE-maps it in the
 * parent. Assert that partial MADV_COLD leaves the shared physical THP and
 * its contents intact.
 */
static void test_shared_pte_mapped_madvise_cold(void)
{
	char *mapping = map_aligned_pages(pmd_size);
	int pipefd[2], status;
	pid_t pid;

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	pid = fork_waiting_child(pipefd);

	split_pmd_mapping(mapping);
	if (madvise(mapping, pmd_size / 2, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("MADV_COLD split a shared large folio\n");
	check_memory(mapping, pmd_size);

	close(pipefd[1]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		ksft_exit_fail_msg("child process failed\n");
	munmap(mapping, pmd_size);
	ksft_test_result_pass("MADV_COLD skips a shared PTE-mapped THP\n");
}

/*
 * A full-range PTE walk reaches the mapcount check rather than the partial
 * folio split check. A second mapping must still prevent MADV_COLD from
 * operating on the shared physical THP.
 */
static void test_full_shared_pte_mapped_madvise_cold(void)
{
	char *mapping = map_aligned_pages(pmd_size);
	int pipefd[2], status;
	pid_t pid;

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	pid = fork_waiting_child(pipefd);

	split_pmd_mapping(mapping);
	if (madvise(mapping, pmd_size, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("MADV_COLD split a shared large folio\n");
	check_memory(mapping, pmd_size);

	close(pipefd[1]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		ksft_exit_fail_msg("child process failed\n");
	munmap(mapping, pmd_size);
	ksft_test_result_pass("full MADV_COLD skips a shared PTE-mapped THP\n");
}

/*
 * fork() gives a PMD-mapped THP another mapping. Assert that partial
 * MADV_COLD leaves the shared physical THP and its contents intact.
 */
static void test_shared_pmd_madvise_cold(void)
{
	char *mapping = map_aligned_pages(pmd_size);
	int pipefd[2], status;
	pid_t pid;

	if (!collapse_all(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		ksft_test_result_skip("could not allocate a PMD-sized THP\n");
		return;
	}
	pid = fork_waiting_child(pipefd);

	if (madvise(mapping, pmd_size / 2, MADV_COLD))
		ksft_exit_fail_perror("MADV_COLD");
	if (!check_large_folios(mapping, pmd_size, 1, pmd_size))
		ksft_exit_fail_msg("MADV_COLD split a shared PMD-mapped THP\n");
	check_memory(mapping, pmd_size);

	close(pipefd[1]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status))
		ksft_exit_fail_msg("child process failed\n");
	munmap(mapping, pmd_size);
	ksft_test_result_pass("MADV_COLD skips a shared PMD-mapped THP\n");
}

static char *map_readonly_file(size_t size, int flags, int *fd)
{
	char template[] = "/tmp/madvise-pageout-XXXXXX";
	char *mapping;

	*fd = mkstemp(template);
	if (*fd < 0)
		ksft_exit_fail_perror("mkstemp");
	if (unlink(template) || ftruncate(*fd, size) || fchmod(*fd, 0400))
		ksft_exit_fail_perror("prepare file");
	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, *fd, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap");

	return mapping;
}

static bool pageout_until_evicted(char *mapping, size_t size, int pagemap_fd)
{
	int retry;

	for (retry = 0; retry < 100; retry++) {
		if (madvise(mapping, size, MADV_PAGEOUT))
			ksft_exit_fail_perror("MADV_PAGEOUT");
		if (!pagemap_is_populated(pagemap_fd, mapping))
			return true;
		usleep(10000);
	}
	return false;
}

/*
 * The owner of a file may page out its clean page-cache pages. Assert that
 * MADV_PAGEOUT removes the populated PTE and preserves the file contents.
 */
static void test_pageout_file(void)
{
	const size_t page_size = getpagesize();
	char *mapping;
	int pagemap_fd;
	bool evicted;
	int fd;

	mapping = map_readonly_file(page_size, MAP_PRIVATE, &fd);
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open pagemap");
	if (mapping[0])
		ksft_exit_fail_msg("new file is not zero-filled\n");

	evicted = pageout_until_evicted(mapping, page_size, pagemap_fd);
	if (mapping[0])
		ksft_exit_fail_msg("file mapping contents changed\n");

	close(pagemap_fd);
	munmap(mapping, page_size);
	close(fd);
	if (!evicted) {
		ksft_test_result_skip("MADV_PAGEOUT did not evict the file page\n");
		return;
	}
	ksft_test_result_pass("MADV_PAGEOUT evicts an authorized file page\n");
}

static int pageout_shared_file_without_permission(char *mapping,
						  size_t page_size)
{
	int pagemap_fd;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		return KSFT_FAIL;
	if (setgid(65534) || setuid(65534))
		return KSFT_FAIL;
	if (mapping[0] || !pagemap_is_populated(pagemap_fd, mapping))
		return KSFT_FAIL;
	if (madvise(mapping, page_size, MADV_PAGEOUT))
		return KSFT_FAIL;
	if (!pagemap_is_populated(pagemap_fd, mapping) || mapping[0])
		return KSFT_FAIL;
	return KSFT_PASS;
}

/*
 * A caller without file write permission may not page out a shared file
 * mapping. Assert that MADV_PAGEOUT succeeds without evicting its file page.
 */
static void test_pageout_unauthorized_shared_file(void)
{
	const size_t page_size = getpagesize();
	char *mapping;
	int fd, status;
	pid_t pid;

	if (geteuid()) {
		ksft_test_result_skip("requires root to change credentials\n");
		return;
	}

	mapping = map_readonly_file(page_size, MAP_SHARED, &fd);

	pid = fork();
	if (pid < 0)
		ksft_exit_fail_perror("fork");
	if (!pid)
		_exit(pageout_shared_file_without_permission(mapping, page_size));

	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != KSFT_PASS)
		ksft_exit_fail_msg("unauthorized child evicted a shared file page\n");
	munmap(mapping, page_size);
	close(fd);
	ksft_test_result_pass("MADV_PAGEOUT skips an unauthorized shared file\n");
}

/*
 * An unauthorized private mapping may page out anonymous COW pages, but not
 * its file folios. Assert that partial MADV_PAGEOUT filters a large shmem
 * folio before attempting to split or reclaim it.
 */
static void test_pageout_anon_only_large_folio(void)
{
	char *mapping, *shared;
	gid_t old_egid;
	int fd;

	if (geteuid()) {
		ksft_test_result_skip("requires root to change credentials\n");
		return;
	}
	fd = memfd_create("madvise-pageout-large", 0);
	if (fd < 0)
		ksft_exit_fail_perror("memfd_create");
	if (ftruncate(fd, pmd_size))
		ksft_exit_fail_perror("ftruncate");
	shared = map_aligned_file(fd, pmd_size, MAP_SHARED);
	memset(shared, 1, pmd_size);
	if (msync(shared, pmd_size, MS_SYNC))
		ksft_exit_fail_perror("msync");
	munmap(shared, pmd_size);

	mapping = map_aligned_file(fd, pmd_size, MAP_PRIVATE);
	if (!collapse_shmem(mapping, pmd_size, 1)) {
		munmap(mapping, pmd_size);
		close(fd);
		ksft_test_result_skip("could not allocate a PMD-sized shmem THP\n");
		return;
	}
	if (fchmod(fd, 0400))
		ksft_exit_fail_perror("fchmod");

	/* Retain saved uid 0 so this process can restore its credentials. */
	old_egid = getegid();
	if (setegid(65534) || seteuid(65534))
		ksft_exit_fail_perror("drop privileges");
	if (madvise(mapping, pmd_size / 2, MADV_PAGEOUT))
		ksft_exit_fail_perror("MADV_PAGEOUT");
	if (seteuid(0) || setegid(old_egid))
		ksft_exit_fail_perror("restore privileges");

	if (!check_huge_shmem(mapping, pmd_size, 1, pmd_size) ||
	    mapping[0] != 1 || mapping[pmd_size - 1] != 1)
		ksft_exit_fail_msg("MADV_PAGEOUT changed a protected large folio\n");
	munmap(mapping, pmd_size);
	close(fd);
	ksft_test_result_pass("MADV_PAGEOUT filters a protected large folio\n");
}

/*
 * A private file mapping can contain both anonymous COW and file-backed
 * pages. As a caller without file write permission, assert that PAGEOUT swaps
 * the COW page but leaves the file-backed page resident.
 */
static void test_pageout_anon_only(void)
{
	const size_t page_size = getpagesize();
	char *mapping;
	int pagemap_fd;
	bool swapped;
	int fd;

	if (geteuid()) {
		ksft_test_result_skip("requires root to change credentials\n");
		return;
	}

	mapping = map_readonly_file(2 * page_size, MAP_PRIVATE, &fd);
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open pagemap");

	if (setgid(65534) || setuid(65534))
		ksft_exit_fail_perror("drop privileges");
	if (mapping[0] || mapping[page_size])
		ksft_exit_fail_msg("new file is not zero-filled\n");
	mapping[0] = 1;
	if (madvise(mapping, 2 * page_size, MADV_PAGEOUT))
		ksft_exit_fail_perror("MADV_PAGEOUT");
	if (!pagemap_is_populated(pagemap_fd, mapping + page_size))
		ksft_exit_fail_msg("MADV_PAGEOUT evicted a protected file page\n");
	swapped = pagemap_is_swapped(pagemap_fd, mapping);
	if (mapping[0] != 1 || mapping[page_size])
		ksft_exit_fail_msg("private file mapping contents changed\n");

	close(pagemap_fd);
	munmap(mapping, 2 * page_size);
	close(fd);
	if (!swapped) {
		ksft_test_result_skip("MADV_PAGEOUT did not swap the COW page\n");
		return;
	}
	ksft_test_result_pass("MADV_PAGEOUT filters private file pages\n");
}

int main(void)
{
	pmd_size = read_pmd_pagesize();

	ksft_print_header();
	ksft_set_plan(28);
	if (!pmd_size)
		ksft_exit_skip("PMD-sized THPs are not supported\n");

	test_full_pmd_madvise_cold();
	test_repeated_pmd_madvise_cold();
	test_partial_pmd_madvise_cold();
	test_pte_mapped_madvise_cold();
	test_madvise_cold_pte_hole();
	test_madvise_cold_zero_page();
	test_madvise_cold_huge_zero_page();
	test_madvise_lru_locked_vma();
	test_shared_pmd_madvise_cold();
	test_full_pmd_madvise_pageout();
	test_madvise_cold_swapped_pte();
	test_partial_pinned_pmd_madvise_cold();
	test_partial_pinned_pte_madvise_cold();
	test_concurrent_partial_madvise_cold();
	test_madvise_pageout_unevictable();
	test_madvise_pageout_unevictable_thp();
	test_madvise_pageout_migration();
	test_madvise_cold_active_folio();
	test_madvise_cold_remote_lru_batch();
	test_pmd_madvise_cold_active_folio();
	test_partial_pmd_madvise_pageout();
	test_partial_pte_madvise_pageout();
	test_shared_pte_mapped_madvise_cold();
	test_full_shared_pte_mapped_madvise_cold();
	test_pageout_file();
	test_pageout_unauthorized_shared_file();
	test_pageout_anon_only_large_folio();
	test_pageout_anon_only();
	ksft_finished();
}
