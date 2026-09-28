// SPDX-License-Identifier: GPL-2.0
/*
 * userfaultfd MISSING + WP test, modelled on a garbage collector.
 *
 * An anonymous region is registered with userfaultfd in both MISSING and
 * WP modes.  A handler thread plays the part of the GC runtime:
 *
 *  - On a missing fault it fills the page from a backing store with
 *    UFFDIO_COPY.  (A real GC might keep evicted pages compressed on disk;
 *    here the backing store is an in-memory buffer with distinct contents
 *    per page, so a page filled with the wrong data is detected.)
 *
 *  - On a write-protect fault it records the page as dirty and removes
 *    write protection from that page only.  This is the userfaultfd
 *    equivalent of mprotect(PROT_READ) plus bookkeeping in a SIGSEGV
 *    handler, as used for GC write barriers.
 *
 * The main thread checks that:
 *  - reads of missing pages are resolved with the right contents,
 *  - a write to a missing page is resolved before the write lands,
 *  - a write to a write-protected page is reported with the WP flag and
 *    then completes,
 *  - write protection is tracked and resolved per page,
 *  - UFFDIO_UNREGISTER leaves populated contents intact and stops
 *    delivering faults for the range.
 *
 * Like the GC it models, the handler assumes pages are only write-protected
 * after they have been populated.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/userfaultfd.h>

#include "../kselftest.h"

#define NR_PAGES 4
#define NR_TESTS 7

/* GC bookkeeping, updated by the handler thread. */
struct page_stats {
	unsigned int missing_faults;
	unsigned int wp_faults;
	uint64_t last_flags;
};

static size_t page_size;
static size_t area_len;
static int uffd;
static int stop_fd;
static char *area;
static char *backing;/* what the handler fills missing pages from */
static char *expected;/* what each page should contain right now */
static struct page_stats stats[NR_PAGES];

#define LOAD(x)__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define INC(x)__atomic_add_fetch(&(x), 1, __ATOMIC_RELEASE)

static uint64_t pattern(unsigned long page, unsigned long word)
{
	return 0xdeadbeef00000000ULL | (page << 16) | word;
}

static uint64_t *page_ptr(unsigned long page)
{
	return (uint64_t *)(area + page * page_size);
}

/* Write a word in the region and record it in the expected image. */
static void store(unsigned long page, unsigned long word, uint64_t val)
{
	page_ptr(page)[word] = val;
	((uint64_t *)(expected + page * page_size))[word] = val;
}

static void handle_missing(unsigned long page)
{
	struct uffdio_copy copy = {
		.dst = (unsigned long)area + page * page_size,
		.src = (unsigned long)backing + page * page_size,
		.len = page_size,
		.mode = 0,
	};
	struct uffdio_range range = {
		.start = copy.dst,
		.len = page_size,
	};

	if (!ioctl(uffd, UFFDIO_COPY, &copy))
		return;
	if (errno != EEXIST)
		ksft_exit_fail_msg("UFFDIO_COPY page %lu: %s\n",
				   page, strerror(errno));
	/* Populated concurrently; the faulting thread still needs a wake. */
	if (ioctl(uffd, UFFDIO_WAKE, &range))
		ksft_exit_fail_msg("UFFDIO_WAKE page %lu: %s\n",
				   page, strerror(errno));
}

static void handle_wp(unsigned long page)
{
	struct uffdio_writeprotect wp = {
		.range = {
			.start = (unsigned long)area + page * page_size,
			.len = page_size,
		},
		.mode = 0,/* clear WP; also wakes the faulting thread */
	};

	if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp))
		ksft_exit_fail_msg("UFFDIO_WRITEPROTECT clear page %lu: %s\n",
				   page, strerror(errno));
}

static void *fault_handler(void *arg)
{
	struct pollfd pfd[2] = {
		{ .fd = uffd, .events = POLLIN },
		{ .fd = stop_fd, .events = POLLIN },
	};
	struct uffd_msg msg;
	unsigned long addr, page;
	uint64_t flags;
	ssize_t ret;

	for (;;) {
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			ksft_exit_fail_msg("poll: %s\n", strerror(errno));
		}
		if (pfd[1].revents & POLLIN)
			return NULL;
		if (pfd[0].revents & POLLERR)
			ksft_exit_fail_msg("POLLERR on userfaultfd\n");
		if (!(pfd[0].revents & POLLIN))
			continue;

		ret = read(uffd, &msg, sizeof(msg));
		if (ret < 0) {
			if (errno == EAGAIN)
				continue;
			ksft_exit_fail_msg("read userfaultfd: %s\n",
					   strerror(errno));
		}
		if (ret != sizeof(msg))
			ksft_exit_fail_msg("short read from userfaultfd: %zd\n",
					   ret);
		if (msg.event != UFFD_EVENT_PAGEFAULT)
			ksft_exit_fail_msg("unexpected userfaultfd event %u\n",
					   msg.event);

		addr = msg.arg.pagefault.address & ~(page_size - 1);
		if (addr < (unsigned long)area ||
		    addr >= (unsigned long)area + area_len)
			ksft_exit_fail_msg("fault at %#lx outside region\n",
					   addr);
		page = (addr - (unsigned long)area) / page_size;
		flags = msg.arg.pagefault.flags;

		/* Record before resolving, so the faulter sees it on wake. */
		STORE(stats[page].last_flags, flags);
		if (flags & UFFD_PAGEFAULT_FLAG_WP) {
			INC(stats[page].wp_faults);
			handle_wp(page);
		} else {
			INC(stats[page].missing_faults);
			handle_missing(page);
		}
	}
}

static bool check_stats(unsigned long page, unsigned int missing,
			unsigned int wp)
{
	unsigned int m = LOAD(stats[page].missing_faults);
	unsigned int w = LOAD(stats[page].wp_faults);

	if (m == missing && w == wp)
		return true;
	ksft_print_msg("page %lu: %u missing / %u wp faults, expected %u / %u\n",
		       page, m, w, missing, wp);
	return false;
}

static bool check_flags(unsigned long page, uint64_t want)
{
	uint64_t mask = UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP;
	uint64_t got = LOAD(stats[page].last_flags) & mask;

	if (got == want)
		return true;
	ksft_print_msg("page %lu: fault flags %#llx, expected %#llx\n", page,
		       (unsigned long long)got, (unsigned long long)want);
	return false;
}

static bool check_page(unsigned long page)
{
	const uint64_t *want = (uint64_t *)(expected + page * page_size);
	unsigned long i;
	uint64_t got;

	for (i = 0; i < page_size / sizeof(uint64_t); i++) {
		got = page_ptr(page)[i];
		if (got != want[i]) {
			ksft_print_msg("page %lu word %lu: %#llx, expected %#llx\n",
				       page, i, (unsigned long long)got,
				       (unsigned long long)want[i]);
			return false;
		}
	}
	return true;
}

static void test_read_missing(unsigned long page)
{
	bool ok;

	(void)page_ptr(page)[0];
	ok = check_stats(page, 1, 0);
	ok &= check_flags(page, 0);
	ok &= check_page(page);
	ksft_test_result(ok, "read fault fills missing page %lu\n", page);
}

static void test_write_missing(void)
{
	bool ok;

	store(1, 0, 0x42);
	ok = check_stats(1, 1, 0);
	ok &= check_flags(1, UFFD_PAGEFAULT_FLAG_WRITE);
	ok &= check_page(1);
	ksft_test_result(ok, "write fault fills missing page 1 before the write\n");
}

static void test_write_protected(void)
{
	struct uffdio_writeprotect wp = {
		.range = { .start = (unsigned long)area, .len = area_len },
		.mode = UFFDIO_WRITEPROTECT_MODE_WP,
	};
	bool ok;

	if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp))
		ksft_exit_fail_msg("UFFDIO_WRITEPROTECT: %s\n", strerror(errno));

	store(0, 0, 0x43);
	ok = check_stats(0, 1, 1);
	ok &= check_flags(0, UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP);
	ok &= check_page(0);
	ksft_test_result(ok, "write to write-protected page 0 is reported, then completes\n");
}

static void test_wp_per_page(void)
{
	bool ok;

	store(0, 1, 0x45);/* page 0 already unprotected: no new fault */
	store(1, 2, 0x44);/* page 1 still protected: its own fault */
	ok = check_stats(0, 1, 1);
	ok &= check_stats(1, 1, 1);
	ok &= check_flags(1, UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP);
	ok &= check_stats(2, 1, 0);/* never written: not dirty */
	ok &= check_page(0);
	ok &= check_page(1);
	ksft_test_result(ok, "write protection is tracked and resolved per page\n");
}

static void test_unregister(void)
{
	struct uffdio_range range = {
		.start = (unsigned long)area,
		.len = area_len,
	};
	unsigned long page;
	bool ok = true;

	if (ioctl(uffd, UFFDIO_UNREGISTER, &range))
		ksft_exit_fail_msg("UFFDIO_UNREGISTER: %s\n", strerror(errno));

	for (page = 0; page < 3; page++)
		ok &= check_page(page);
	ksft_test_result(ok, "populated contents survive UFFDIO_UNREGISTER\n");
}

static void test_after_unregister(void)
{
	bool ok;

	/*
	 * Page 3 was never touched.  If the fault were still routed to us,
	 * the handler would fill it from the backing store.
	 */
	ok = check_page(3);
	ok &= check_stats(3, 0, 0);
	ksft_test_result(ok, "untouched page is zero-filled after unregister\n");
}

static int uffd_open(uint64_t features, uint64_t *supported)
{
	struct uffdio_api api = { .api = UFFD_API, .features = features };
	int fd;

	fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
#ifdef UFFD_USER_MODE_ONLY
	if (fd < 0 && errno == EPERM)
		fd = syscall(__NR_userfaultfd,
			     O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
#endif
	if (fd < 0) {
		if (errno == ENOSYS || errno == EPERM)
			ksft_exit_skip("userfaultfd unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("userfaultfd: %s\n", strerror(errno));
	}
	if (ioctl(fd, UFFDIO_API, &api))
		ksft_exit_fail_msg("UFFDIO_API: %s\n", strerror(errno));
	if (supported)
		*supported = api.features;
	return fd;
}

int main(void)
{
	uint64_t required = (1ULL << _UFFDIO_COPY) | (1ULL << _UFFDIO_WAKE) |
		(1ULL << _UFFDIO_WRITEPROTECT);
	struct uffdio_register reg;
	unsigned long page, i;
	uint64_t features;
	pthread_t thread;
	int ret;

	ksft_print_header();

	page_size = sysconf(_SC_PAGESIZE);
	area_len = NR_PAGES * page_size;

	/* UFFDIO_API can only be issued once per fd: probe on a throwaway. */
	close(uffd_open(0, &features));
	if (!(features & UFFD_FEATURE_PAGEFAULT_FLAG_WP))
		ksft_exit_skip("userfaultfd write-protect not supported\n");
	uffd = uffd_open(UFFD_FEATURE_PAGEFAULT_FLAG_WP, NULL);

	area = mmap(NULL, area_len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));

	backing = malloc(area_len);
	expected = malloc(area_len);
	if (!backing || !expected)
		ksft_exit_fail_msg("malloc failed\n");
	for (page = 0; page < NR_PAGES; page++)
		for (i = 0; i < page_size / sizeof(uint64_t); i++)
			((uint64_t *)(backing + page * page_size))[i] =
				pattern(page, i);
	memcpy(expected, backing, area_len);
	/* Page 3 is only read after unregister: expect a fresh zero page. */
	memset(expected + 3 * page_size, 0, page_size);

	reg.range.start = (unsigned long)area;
	reg.range.len = area_len;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
	if (ioctl(uffd, UFFDIO_REGISTER, &reg))
		ksft_exit_fail_msg("UFFDIO_REGISTER: %s\n", strerror(errno));
	if ((reg.ioctls & required) != required)
		ksft_exit_fail_msg("missing range ioctls: have %#llx, need %#llx\n",
				   (unsigned long long)reg.ioctls,
				   (unsigned long long)required);

	stop_fd = eventfd(0, EFD_CLOEXEC);
	if (stop_fd < 0)
		ksft_exit_fail_msg("eventfd: %s\n", strerror(errno));
	ret = pthread_create(&thread, NULL, fault_handler, NULL);
	if (ret)
		ksft_exit_fail_msg("pthread_create: %s\n", strerror(ret));

	ksft_set_plan(NR_TESTS);

	test_read_missing(0);
	test_read_missing(2);
	test_write_missing();
	test_write_protected();
	test_wp_per_page();
	test_unregister();
	test_after_unregister();

	if (eventfd_write(stop_fd, 1))
		ksft_exit_fail_msg("eventfd_write: %s\n", strerror(errno));
	pthread_join(thread, NULL);

	close(stop_fd);
	close(uffd);
	munmap(area, area_len);
	free(backing);
	free(expected);

	ksft_finished();
}
