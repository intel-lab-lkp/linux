// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SWAP_FLAG_PREFER	0x8000
#define SWAP_FLAG_DISCARD	0x10000
#define SWAP_FLAG_DISCARD_ONCE	0x20000
#define SWAP_FLAG_DISCARD_PAGES	0x40000
#define SWAP_FLAG_OFFLOAD_ONLY	0x80000
#define KSFT_SKIP	4

static int activate(const char *path, int priority, int discard_flags)
{
	int flags = SWAP_FLAG_PREFER | SWAP_FLAG_OFFLOAD_ONLY |
		priority | discard_flags;

	if (syscall(SYS_swapon, path, flags)) {
		perror("swapon");
		return 1;
	}

	return 0;
}

static int reject_page_discard(const char *path, int priority)
{
	int flags = SWAP_FLAG_PREFER | SWAP_FLAG_OFFLOAD_ONLY |
		SWAP_FLAG_DISCARD | SWAP_FLAG_DISCARD_PAGES | priority;
	int ret;

	errno = 0;
	ret = syscall(SYS_swapon, path, flags);
	if (ret == -1 && errno == EINVAL)
		return 0;
	if (!ret) {
		syscall(SYS_swapoff, path);
		fprintf(stderr, "offload-only page discard was accepted\n");
	} else {
		fprintf(stderr, "swapon returned unexpected error: %s\n",
			strerror(errno));
	}
	return 1;
}

static int accept_discard_once_pages(const char *path, int priority)
{
	int flags = SWAP_FLAG_PREFER | SWAP_FLAG_OFFLOAD_ONLY |
		SWAP_FLAG_DISCARD | SWAP_FLAG_DISCARD_ONCE |
		SWAP_FLAG_DISCARD_PAGES | priority;

	if (syscall(SYS_swapon, path, flags)) {
		perror("swapon discard-once+discard-pages");
		return 1;
	}
	if (syscall(SYS_swapoff, path)) {
		perror("swapoff discard-once+discard-pages");
		return 1;
	}
	return 0;
}

static int join_cgroup(const char *procs)
{
	char pid[32];
	int fd, len;

	fd = open(procs, O_WRONLY);
	if (fd < 0) {
		perror("open cgroup.procs");
		return 1;
	}

	len = snprintf(pid, sizeof(pid), "%d\n", getpid());
	if (write(fd, pid, len) != len) {
		perror("write cgroup.procs");
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

static int pin_to_one_cpu(const char *pid_arg)
{
	cpu_set_t allowed, selected;
	char *end;
	long pid;
	int cpu;

	errno = 0;
	pid = strtol(pid_arg, &end, 10);
	if (errno || *end || pid <= 0) {
		fprintf(stderr, "invalid pid: %s\n", pid_arg);
		return 1;
	}

	if (sched_getaffinity(pid, sizeof(allowed), &allowed)) {
		perror("sched_getaffinity");
		return 1;
	}
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &allowed))
			break;
	if (cpu == CPU_SETSIZE) {
		fprintf(stderr, "pid %ld has no allowed CPU\n", pid);
		return 1;
	}

	CPU_ZERO(&selected);
	CPU_SET(cpu, &selected);
	if (sched_setaffinity(pid, sizeof(selected), &selected)) {
		perror("sched_setaffinity");
		return 1;
	}
	return 0;
}

static int allocate(const char *size_arg, const char *procs,
		    const char *ready, const char *verified)
{
	unsigned char *memory;
	unsigned long size;
	unsigned long page_size;
	char *end;
	sigset_t signals;
	int signal;
	int fd;

	errno = 0;
	size = strtoul(size_arg, &end, 0);
	if (errno || *end || !size) {
		fprintf(stderr, "invalid allocation size: %s\n", size_arg);
		return 1;
	}
	if (join_cgroup(procs))
		return 1;

	memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	page_size = getpagesize();
	for (unsigned long i = 0; i < size; i += page_size)
		memset(memory + i, i / page_size % 251 + 1,
		       page_size < size - i ? page_size : size - i);
	if (mprotect(memory, size, PROT_READ)) {
		perror("mprotect");
		return 1;
	}

	sigemptyset(&signals);
	sigaddset(&signals, SIGUSR1);
	if (sigprocmask(SIG_BLOCK, &signals, NULL)) {
		perror("sigprocmask");
		return 1;
	}

	fd = open(ready, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		perror("create ready file");
		return 1;
	}
	close(fd);

	errno = sigwait(&signals, &signal);
	if (errno) {
		perror("sigwait");
		return 1;
	}
	for (unsigned long i = 0; i < size; i++) {
		unsigned char expected = i / page_size % 251 + 1;

		if (memory[i] != expected) {
			fprintf(stderr,
				"data mismatch at %lu: got %u, expected %u\n",
				i, memory[i], expected);
			return 1;
		}
	}

	fd = open(verified, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		perror("create verified file");
		return 1;
	}
	close(fd);

	for (;;)
		pause();
}

static int create_marker(const char *path, int write_pid)
{
	char *temporary;
	int fd, ret = 1;

	if (asprintf(&temporary, "%s.XXXXXX", path) < 0) {
		perror("asprintf marker path");
		return 1;
	}
	fd = mkstemp(temporary);
	if (fd < 0) {
		perror(path);
		goto out_free;
	}
	if (write_pid && dprintf(fd, "%d\n", getpid()) < 0) {
		perror("write marker");
		close(fd);
		goto out_unlink;
	}
	if (close(fd)) {
		perror("close marker");
		goto out_unlink;
	}
	/* Publish only after the payload is complete for the shell reader. */
	if (rename(temporary, path)) {
		perror("publish marker");
		goto out_unlink;
	}
	ret = 0;
out_unlink:
	unlink(temporary);
out_free:
	free(temporary);
	return ret;
}

static unsigned char retained_byte(unsigned long offset, long page_size,
				   int touched)
{
	unsigned char value = offset / page_size % 251 + 1;

	if (touched && !offset)
		value ^= 0x5a;
	return value;
}

static int verify_retained(unsigned char *memory, unsigned long size,
			   long page_size, int full)
{
	unsigned long limit = full ? size : 1;

	for (unsigned long offset = 0; offset < limit; offset++) {
		unsigned char expected = retained_byte(offset, page_size, 1);

		if (memory[offset] != expected) {
			fprintf(stderr,
				"retained data mismatch at %lu: got %u, expected %u\n",
				offset, memory[offset], expected);
			return 1;
		}
	}
	return 0;
}

static int retained(const char *size_arg, const char *procs,
		    const char *ready, const char *touched,
		    const char *verified)
{
	unsigned long allocation_size, mapping_start, size;
	unsigned char *mapping, *memory;
	char *end;
	sigset_t signals;
	long page_size;
	int signal;

	errno = 0;
	size = strtoul(size_arg, &end, 0);
	if (errno || *end || !size || (size & (size - 1))) {
		fprintf(stderr, "allocation size must be a power of two\n");
		return 1;
	}
	if (join_cgroup(procs))
		return 1;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		perror("sysconf _SC_PAGESIZE");
		return 1;
	}
	if (size < (unsigned long)page_size || size % page_size) {
		fprintf(stderr, "allocation size must contain whole pages\n");
		return 1;
	}
	if (size > ULONG_MAX / 2) {
		fprintf(stderr, "allocation size is too large\n");
		return 1;
	}
	allocation_size = size * 2;
	mapping = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	mapping_start = (unsigned long)mapping;
	memory = (unsigned char *)((mapping_start + size - 1) & ~(size - 1));
	if ((unsigned long)memory != mapping_start)
		munmap((void *)mapping_start,
		       (unsigned long)memory - mapping_start);
	munmap(memory + size, allocation_size - size -
	       ((unsigned long)memory - mapping_start));
	if (madvise(memory, size, MADV_HUGEPAGE)) {
		perror("madvise MADV_HUGEPAGE");
		return 1;
	}
	for (unsigned long offset = 0; offset < size; offset += page_size)
		memset(memory + offset, retained_byte(offset, page_size, 0),
		       page_size);
#ifdef MADV_COLLAPSE
	if (madvise(memory, size, MADV_COLLAPSE)) {
		int error = errno;

		perror("madvise MADV_COLLAPSE");
		if (error == EAGAIN || error == EINVAL || error == ENOMEM)
			return KSFT_SKIP;
		return 1;
	}
#else
	fprintf(stderr, "MADV_COLLAPSE is unavailable\n");
	return KSFT_SKIP;
#endif

	sigemptyset(&signals);
	sigaddset(&signals, SIGUSR1);
	sigaddset(&signals, SIGUSR2);
	sigaddset(&signals, SIGALRM);
	if (sigprocmask(SIG_BLOCK, &signals, NULL)) {
		perror("sigprocmask");
		return 1;
	}
	if (create_marker(ready, 1))
		return 1;

	for (;;) {
		errno = sigwait(&signals, &signal);
		if (errno) {
			perror("sigwait");
			return 1;
		}
		if (signal == SIGUSR1) {
			unsigned char value;

			if (mprotect(memory, page_size, PROT_READ)) {
				perror("mprotect read");
				return 1;
			}
			value = memory[0];
			if (mprotect(memory, page_size, PROT_READ | PROT_WRITE)) {
				perror("mprotect write");
				return 1;
			}
			memory[0] = value ^ 0x5a;
			if (create_marker(touched, 0))
				return 1;
		} else {
			if (verify_retained(memory, size, page_size,
					    signal == SIGALRM))
				return 1;
			if (create_marker(verified, 0))
				return 1;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "activate"))
		return activate(argv[2], atoi(argv[3]),
				SWAP_FLAG_DISCARD | SWAP_FLAG_DISCARD_ONCE);
	if (argc == 4 && !strcmp(argv[1], "activate-no-discard"))
		return activate(argv[2], atoi(argv[3]), 0);
	if (argc == 4 && !strcmp(argv[1], "reject-page-discard"))
		return reject_page_discard(argv[2], atoi(argv[3]));
	if (argc == 4 && !strcmp(argv[1], "accept-discard-once-pages"))
		return accept_discard_once_pages(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "pin"))
		return pin_to_one_cpu(argv[2]);
	if (argc == 6 && !strcmp(argv[1], "allocate"))
		return allocate(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 7 && !strcmp(argv[1], "retained"))
		return retained(argv[2], argv[3], argv[4], argv[5], argv[6]);

	fprintf(stderr,
		"usage: %s activate|activate-no-discard DEVICE PRIORITY | reject-page-discard DEVICE PRIORITY | accept-discard-once-pages DEVICE PRIORITY | pin PID | allocate BYTES CGROUP.PROCS READY VERIFIED | retained BYTES CGROUP.PROCS READY TOUCHED VERIFIED\n",
		argv[0]);
	return 1;
}
