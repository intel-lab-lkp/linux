// SPDX-License-Identifier: GPL-2.0
/*
 * Device memory perf callchain unwinding test (arm64 only).
 *
 * Maps a physical address via /dev/mem (creating a device memory mapping),
 * launches perf record to sample this process with frame-pointer
 * callchains, then points FP (x29) into the mapping and spins.
 * The test passes if the kernel survives without crashing.
 *
 * The default MMIO address is 0xc0000000; override via environment:
 *   MMIO_ADDR=0x10000000 ./test_perf_vmio
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest_harness.h"

#define DEFAULT_MMIO_ADDR 0xc0000000UL

TEST(device_memory_callchain)
{
#ifndef __aarch64__
	SKIP(return, "arm64 only");
#else
	unsigned long pa = DEFAULT_MMIO_ADDR;
	unsigned long page, off;
	pid_t spin_pid, perf_pid;
	char pid_str[16];
	int fd, pst;
	void *m, *fp;
	char *env;

	if (getuid() != 0)
		SKIP(return, "need root");

	env = getenv("MMIO_ADDR");
	if (env)
		pa = strtoul(env, NULL, 16);

	page = pa & ~0xFFFUL;
	off = pa - page;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		SKIP(return, "cannot open /dev/mem");

	/* Fork a spinner child with FP pointing into device memory */
	spin_pid = fork();
	ASSERT_GE(spin_pid, 0);
	if (spin_pid == 0) {
		/*
		 * mmap /dev/mem in the child so remap_pfn_range populates
		 * PTEs directly. fork() does not copy PTEs for VM_PFNMAP
		 * regions, so mapping before fork leaves the child with
		 * empty page tables — the unwinder would get a translation
		 * fault instead of a synchronous external abort.
		 */
		m = mmap(NULL, off + 4096, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, page);
		if (m == MAP_FAILED)
			_exit(1);
		fp = (char *)m + off;
		__asm__ volatile(
			"mov x29, %0\n"
			"1: b 1b\n"
			: : "r"(fp) : "x29", "memory");
		_exit(0);
	}

	/* Launch perf to sample the spinner */
	snprintf(pid_str, sizeof(pid_str), "%d", spin_pid);

	perf_pid = fork();
	if (perf_pid < 0) {
		kill(spin_pid, SIGKILL);
		waitpid(spin_pid, NULL, 0);
		close(fd);
		ASSERT_GE(perf_pid, 0);
	}
	if (perf_pid == 0) {
		char *const perf_argv[] = {
			"perf", "record", "-g", "--call-graph", "fp",
			"-p", pid_str, "--", "sleep", "3", NULL
		};

		if (chdir("/tmp"))
			_exit(1);
		execvp(perf_argv[0], perf_argv);
		_exit(1);
	}

	waitpid(perf_pid, &pst, 0);

	kill(spin_pid, SIGKILL);
	waitpid(spin_pid, NULL, 0);
	close(fd);

	if (WIFEXITED(pst) && WEXITSTATUS(pst) == 1)
		SKIP(return, "perf not available");

	/*
	 * The real test is that the kernel survived. If we got here
	 * without a synchronous external abort, the guard worked.
	 */
	TH_LOG("kernel survived perf sampling with FP in device memory");
#endif /* __aarch64__ */
}

TEST_HARNESS_MAIN
