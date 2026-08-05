// SPDX-License-Identifier: GPL-2.0
/*
 * Test that memory failure can split a mappingless shmem THP in swap cache.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../kselftest.h"
#include "hugepage_settings.h"
#include "vm_util.h"

#define HWPOISON_PATH "/sys/kernel/debug/hwpoison/corrupt-pfn"

#define KPF_MMAP	BIT_ULL(11)
#define KPF_SWAPCACHE	BIT_ULL(13)
#define KPF_SWAPBACKED	BIT_ULL(14)

#define TEST_BYTE	0x5a
#define PAGEOUT_RETRIES	100
#define PAGEOUT_DELAY_US	100000

static bool is_swapcache_thp(uint64_t flags, bool head)
{
	uint64_t required = KPF_SWAPCACHE | KPF_SWAPBACKED | KPF_THP;

	required |= head ? KPF_COMPOUND_HEAD : KPF_COMPOUND_TAIL;
	return (flags & required) == required && !(flags & KPF_MMAP);
}

static int wait_for_swapcache_thp(unsigned long head_pfn, unsigned long target_pfn,
				  int kpageflags_fd)
{
	uint64_t head_flags = 0;
	uint64_t target_flags = 0;
	int i;

	for (i = 0; i < PAGEOUT_RETRIES; i++) {
		if (pageflags_get(head_pfn, kpageflags_fd, &head_flags) ||
		    pageflags_get(target_pfn, kpageflags_fd, &target_flags))
			return -1;

		if (is_swapcache_thp(head_flags, true) &&
		    is_swapcache_thp(target_flags, false))
			return 0;

		usleep(PAGEOUT_DELAY_US);
	}

	ksft_print_msg("Swapcache THP flags: head=%#llx target=%#llx\n",
			(unsigned long long)head_flags,
			(unsigned long long)target_flags);
	return 1;
}

static bool folio_was_split(unsigned long head_pfn, unsigned long target_pfn,
			    unsigned long nr_pages, int kpageflags_fd)
{
	const uint64_t compound = KPF_COMPOUND_HEAD | KPF_COMPOUND_TAIL;
	uint64_t flags;
	unsigned long i;

	for (i = 0; i < nr_pages; i++) {
		if (pageflags_get(head_pfn + i, kpageflags_fd, &flags))
			return false;
		if (flags & compound) {
			ksft_print_msg("PFN %#lx is still compound (flags=%#llx)\n",
					head_pfn + i,
					(unsigned long long)flags);
			return false;
		}
		if ((head_pfn + i == target_pfn) != !!(flags & KPF_HWPOISON)) {
			ksft_print_msg(
				"Unexpected HWPoison state at PFN %#lx (flags=%#llx)\n",
				head_pfn + i, (unsigned long long)flags);
			return false;
		}
	}

	return true;
}

static bool mapping_has_expected_data(const unsigned char *addr, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (addr[i] != TEST_BYTE) {
			ksft_print_msg("Data mismatch at offset %#zx: %#x != %#x\n",
					i, addr[i], TEST_BYTE);
			return false;
		}
	}

	return true;
}

static int inject_hwpoison(unsigned long pfn)
{
	char buf[32];
	ssize_t written;
	int fd;
	int len;
	int saved_errno;

	fd = open(HWPOISON_PATH, O_WRONLY);
	if (fd < 0)
		return -errno;

	len = snprintf(buf, sizeof(buf), "%#lx\n", pfn);
	written = write(fd, buf, len);
	saved_errno = errno;
	close(fd);

	if (written != len)
		return written < 0 ? -saved_errno : -EIO;

	return 0;
}

int main(void)
{
	struct thp_settings settings;
	unsigned long target_pfn;
	unsigned long head_pfn;
	unsigned long nr_pages;
	unsigned long page_size;
	unsigned long pmd_size;
	unsigned char *mapping;
	unsigned char *addr;
	uint64_t flags;
	bool poisoned = false;
	bool pass = false;
	int kpageflags_fd = -1;
	int pagemap_fd = -1;
	int memfd = -1;
	int ret;

	ksft_print_header();
	ksft_set_plan(1);

	if (geteuid())
		ksft_exit_skip("Please run the test as root\n");

	pmd_size = read_pmd_pagesize();
	if (!thp_available() || !pmd_size)
		ksft_exit_skip("Transparent Huge Pages are not available\n");

	if (access(HWPOISON_PATH, W_OK))
		ksft_exit_skip("HWPoison injection is not available\n");

	page_size = getpagesize();
	if (pmd_size % page_size)
		ksft_exit_fail_msg("Invalid PMD page size %#lx\n", pmd_size);
	nr_pages = pmd_size / page_size;

	thp_save_settings();
	thp_read_settings(&settings);
	settings.shmem_enabled = SHMEM_ADVISE;
	thp_write_settings(&settings);

	memfd = memfd_create("split_hwpoison_swapcache", MFD_CLOEXEC);
	if (memfd < 0)
		ksft_exit_fail_perror("memfd_create");
	if (ftruncate(memfd, pmd_size))
		ksft_exit_fail_perror("ftruncate");

	/* Reserve enough space to obtain a PMD-aligned file mapping. */
	mapping = mmap(NULL, 2 * pmd_size, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_perror("mmap");
	addr = (unsigned char *)(((uintptr_t)mapping + pmd_size - 1) &
				 ~(pmd_size - 1));
	if (mmap(addr, pmd_size, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED, memfd, 0) == MAP_FAILED)
		ksft_exit_fail_perror("mmap");

	if (madvise(addr, pmd_size, MADV_HUGEPAGE))
		ksft_exit_fail_perror("madvise(MADV_HUGEPAGE)");
	memset(addr, TEST_BYTE, pmd_size);

	if (!check_huge_shmem(addr, 1, pmd_size))
		ksft_exit_skip("Failed to allocate a PMD-sized shmem THP\n");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_perror("open(/proc/self/pagemap)");
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY);
	if (kpageflags_fd < 0)
		ksft_exit_fail_perror("open(/proc/kpageflags)");

	head_pfn = pagemap_get_pfn(pagemap_fd, (char *)addr);
	if (head_pfn == -1UL)
		ksft_exit_fail_msg("Failed to obtain the shmem THP PFN\n");

	/* Poison a tail page so success necessarily requires a real split. */
	target_pfn = head_pfn + nr_pages / 2;
	if (pageflags_get(head_pfn, kpageflags_fd, &flags) ||
	    (flags & (KPF_THP | KPF_COMPOUND_HEAD)) !=
		    (KPF_THP | KPF_COMPOUND_HEAD))
		ksft_exit_fail_msg("PFN %#lx is not a THP head\n", head_pfn);
	if (pageflags_get(target_pfn, kpageflags_fd, &flags) ||
	    (flags & (KPF_THP | KPF_COMPOUND_TAIL)) !=
		    (KPF_THP | KPF_COMPOUND_TAIL))
		ksft_exit_fail_msg("PFN %#lx is not a THP tail\n", target_pfn);

	if (madvise(addr, pmd_size, MADV_PAGEOUT))
		ksft_exit_skip("madvise(MADV_PAGEOUT) failed: %s\n",
			       strerror(errno));

	ret = wait_for_swapcache_thp(head_pfn, target_pfn, kpageflags_fd);
	if (ret < 0)
		ksft_exit_fail_msg("Failed to read kpageflags\n");
	if (ret > 0)
		ksft_exit_skip("Failed to create a mappingless swapcache THP; "
			       "is swap enabled?\n");

	ksft_print_msg("Injecting HWPoison into tail PFN %#lx of THP %#lx\n",
			target_pfn, head_pfn);
	ret = inject_hwpoison(target_pfn);
	poisoned = true;
	if (ret) {
		ksft_print_msg("HWPoison injection failed: %s\n", strerror(-ret));
		goto out;
	}

	if (!folio_was_split(head_pfn, target_pfn, nr_pages, kpageflags_fd))
		goto out;

	/*
	 * A clean poisoned swapcache page is discarded. Faulting the mapping
	 * back in must recover the original data from swap.
	 */
	if (!mapping_has_expected_data(addr, pmd_size))
		goto out;

	pass = true;
out:
	if (poisoned && unpoison_memory(target_pfn)) {
		ksft_print_msg("Failed to unpoison PFN %#lx\n", target_pfn);
		pass = false;
	}
	if (kpageflags_fd >= 0)
		close(kpageflags_fd);
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	munmap(mapping, 2 * pmd_size);
	close(memfd);

	ksft_test_result(pass, "memory failure splits a mappingless swapcache THP\n");
	ksft_finished();
}
