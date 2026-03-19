// SPDX-License-Identifier: GPL-2.0
/*
 * This test makes sure when memory failure happens, shmem can handle
 * successfully.
 */
#include <linux/compiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include "kselftest.h"
#include "vm_util.h"

static sigjmp_buf sigbuf;

static void signal_handler(int sig, siginfo_t *info, void *ucontext)
{
	siglongjmp(sigbuf, 1);
}

static void set_signal_handler(int sig, void (*handler)(int, siginfo_t *, void *))
{
	struct sigaction sa = {};

	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, NULL) == -1)
		ksft_exit_fail_msg("Failed to set SIGBUS handler: %s\n", strerror(errno));
}

static unsigned long addr_to_pfn(char *addr)
{
	int pagemap_fd;
	unsigned long pfn;

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		ksft_exit_fail_msg("Failed to open /proc/self/pagemap: %s\n", strerror(errno));
	pfn = pagemap_get_pfn(pagemap_fd, addr);
	close(pagemap_fd);

	return pfn;
}

static void test_shmem_memory_failure(size_t total_size, size_t page_size)
{
	unsigned long memory_failure_pfn;
	char *memory_failure_mem;
	char *memory_failure_addr;
	int fd;

	fd = memfd_create("shmem_hwpoison_test", 0);
	if (fd < 0)
		ksft_exit_skip("memfd_create failed: %s\n", strerror(errno));

	if (ftruncate(fd, total_size) < 0)
		ksft_exit_fail_msg("ftruncate failed: %s\n", strerror(errno));

	memory_failure_mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (memory_failure_mem == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memory_failure_addr = memory_failure_mem + page_size;
	READ_ONCE(memory_failure_addr[0]);
	memory_failure_pfn = addr_to_pfn(memory_failure_addr);

	if (madvise(memory_failure_addr, page_size, MADV_HWPOISON) != 0)
		ksft_exit_fail_msg("MADV_HWPOISON failed: %s\n", strerror(errno));

	if (sigsetjmp(sigbuf, 1) == 0) {
		READ_ONCE(memory_failure_addr[0]);
		ksft_test_result_fail("Read from poisoned page should have triggered SIGBUS\n");
	} else {
		ksft_test_result_pass("SIGBUS triggered as expected on poisoned page\n");
	}

	munmap(memory_failure_mem, total_size);
	close(fd);
	if (unpoison_memory(memory_failure_pfn) < 0)
		ksft_exit_fail_msg("unpoison_memory failed: %s\n", strerror(errno));
}

int main(int argc, char *argv[])
{
	const size_t pagesize = getpagesize();

	ksft_print_header();
	ksft_set_plan(1);

	set_signal_handler(SIGBUS, signal_handler);
	test_shmem_memory_failure(pagesize * 4, pagesize);
	ksft_finished();
}
