// SPDX-License-Identifier: GPL-2.0-only
/*
 * Basic VM_PFNMAP tests relying on mmap() of '/dev/mem'
 *
 * Copyright 2025, Red Hat, Inc.
 *
 * Author(s): David Hildenbrand <david@redhat.com>
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "../kselftest.h"
#include "vm_util.h"

static size_t pagesize;
static int pagemap_fd;
static int dev_mem_fd;
static sigjmp_buf env;

static void signal_handler(int sig)
{
	if (sig == SIGSEGV)
		siglongjmp(env, 1);
	siglongjmp(env, 2);
}

static void sense_support(void)
{
	char *addr, tmp;
	int ret;

	dev_mem_fd = open("/dev/mem", O_RDONLY);
	if (dev_mem_fd < 0)
		ksft_exit_skip("Cannot open '/dev/mem': %s\n", strerror(errno));

	/* We'll require the first two pages throughout our tests ... */
	addr = mmap(0, pagesize * 2, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr == MAP_FAILED)
		ksft_exit_skip("Cannot mmap '/dev/mem'");

	/* ... and want to be able to read from them. */
	ret = sigsetjmp(env, 1);
	if (!ret) {
		tmp = *addr + *(addr + pagesize);
		asm volatile("" : "+r" (tmp));
	}
	if (ret)
		ksft_exit_skip("Cannot read-access mmap'ed '/dev/mem'");

	munmap(addr, pagesize * 2);
}

static void test_madvise(void)
{
#define INIT_ADVICE(nr) { nr, #nr}
	const struct {
		int nr;
		const char *name;
	} advices[] = {
		INIT_ADVICE(MADV_DONTNEED),
		INIT_ADVICE(MADV_DONTNEED_LOCKED),
		INIT_ADVICE(MADV_FREE),
		INIT_ADVICE(MADV_WIPEONFORK),
		INIT_ADVICE(MADV_COLD),
		INIT_ADVICE(MADV_PAGEOUT),
		INIT_ADVICE(MADV_POPULATE_READ),
		INIT_ADVICE(MADV_POPULATE_WRITE),
	};
	char *addr;
	int ret, i;

	addr = mmap(0, pagesize, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* All these advices must be rejected. */
	for (i = 0; i < ARRAY_SIZE(advices); i++) {
		ret = madvise(addr, pagesize, advices[i].nr);
		ksft_test_result(ret && errno == EINVAL,
				 "madvise(%s) should be disallowed\n",
				 advices[i].name);
	}

	munmap(addr, pagesize);
}

static void test_munmap_splitting(void)
{
	char *addr1, *addr2;
	int ret;

	addr1 = mmap(0, pagesize * 2, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr1 == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* Unmap the first pages. */
	ret = munmap(addr1, pagesize);
	ksft_test_result(!ret, "munmap() splitting\n");

	/* Remap the first page while the second page is still mapped. */
	addr2 = mmap(0, pagesize, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	ksft_test_result(addr2 != MAP_FAILED, "mmap() after splitting\n");

	if (addr2 != MAP_FAILED)
		munmap(addr2, pagesize);
	if (!ret)
		munmap(addr1 + pagesize, pagesize);
	else
		munmap(addr1, pagesize * 2);
}

static void test_mremap_fixed(void)
{
	char *addr, *new_addr, *ret;

	addr = mmap(0, pagesize * 2, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* Reserve a destination area. */
	new_addr = mmap(0, pagesize * 2, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (new_addr == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* mremap() over our destination. */
	ret = mremap(addr, pagesize * 2, pagesize * 2,
		     MREMAP_FIXED | MREMAP_MAYMOVE, new_addr);
	ksft_test_result(ret == new_addr, "mremap(MREMAP_FIXED)\n");
	if (ret != new_addr)
		munmap(new_addr, pagesize * 2);
	munmap(addr, pagesize * 2);
}

static void test_mremap_shrinking(void)
{
	char *addr, *ret;

	addr = mmap(0, pagesize * 2, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* Shrinking is expected to work. */
	ret = mremap(addr, pagesize * 2, pagesize, 0);
	ksft_test_result(ret == addr, "mremap() shrinking\n");
	if (ret != addr)
		munmap(addr, pagesize * 2);
	else
		munmap(addr, pagesize);
}

static void test_mremap_growing(void)
{
	char *addr, *ret;

	addr = mmap(0, pagesize, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* Growing is not expected to work. */
	ret = mremap(addr, pagesize, pagesize * 2, MREMAP_MAYMOVE);
	ksft_test_result(ret == MAP_FAILED,
			 "mremap() growing should be disallowed\n");
	if (ret == MAP_FAILED)
		munmap(addr, pagesize);
	else
		munmap(ret, pagesize * 2);
}

static void test_mprotect(void)
{
	char *addr, tmp;
	int ret;

	addr = mmap(0, pagesize, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* With PROT_NONE, read access must result in SIGSEGV. */
	ret = mprotect(addr, pagesize, PROT_NONE);
	ksft_test_result(!ret, "mprotect(PROT_NONE)\n");

	ret = sigsetjmp(env, 1);
	if (!ret) {
		tmp = *addr;
		asm volatile("" : "+r" (tmp));
	}
	ksft_test_result(ret == 1, "SIGSEGV expected\n");

	/* With PROT_READ, read access must again succeed. */
	ret = mprotect(addr, pagesize, PROT_READ);
	ksft_test_result(!ret, "mprotect(PROT_READ)\n");

	ret = sigsetjmp(env, 1);
	if (!ret) {
		tmp = *addr;
		asm volatile("" : "+r" (tmp));
	}
	ksft_test_result(!ret, "SIGSEGV not expected\n");

	munmap(addr, pagesize);
}

static void test_fork(void)
{
	char *addr, tmp;
	int ret;

	addr = mmap(0, pagesize, PROT_READ, MAP_SHARED, dev_mem_fd, 0);
	if (addr == MAP_FAILED)
		ksft_exit_fail_msg("mmap() failed: %s\n", strerror(errno));

	/* fork() a child and test if the child can access the page. */
	ret = fork();
	if (ret < 0) {
		ksft_test_result_fail("fork()\n");
		goto out;
	} else if (!ret) {
		ret = sigsetjmp(env, 1);
		if (!ret) {
			tmp = *addr;
			asm volatile("" : "+r" (tmp));
		}
		/* Return the result to the parent. */
		exit(ret);
	}
	ksft_test_result_pass("fork()\n");

	/* Wait for our child and obtain the result. */
	wait(&ret);
	if (WIFEXITED(ret))
		ret = WEXITSTATUS(ret);
	else
		ret = -EINVAL;

	ksft_test_result(!ret, "SIGSEGV in child not expected\n");
out:
	munmap(addr, pagesize);
}

int main(int argc, char **argv)
{
	int err;

	ksft_print_header();
	ksft_set_plan(19);

	pagesize = getpagesize();
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_msg("opening pagemap failed\n");
	if (signal(SIGSEGV, signal_handler) == SIG_ERR)
		ksft_exit_fail_msg("signal() failed: %s\n", strerror(errno));

	sense_support();
	test_madvise();
	test_munmap_splitting();
	test_mremap_fixed();
	test_mremap_shrinking();
	test_mremap_growing();
	test_mprotect();
	test_fork();

	err = ksft_get_fail_cnt();
	if (err)
		ksft_exit_fail_msg("%d out of %d tests failed\n",
				   err, ksft_test_num());
	ksft_exit_pass();
}
