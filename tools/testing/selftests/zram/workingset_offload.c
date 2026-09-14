// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANON_SIZE (64UL << 20)
#define TARGET_SIZE (16UL << 20)
#define FILLER_SIZE (32UL << 20)

static int join_cgroup(const char *path)
{
	char pid[32];
	int fd, len;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	len = snprintf(pid, sizeof(pid), "%d\n", getpid());
	if (write(fd, pid, len) != len) {
		close(fd);
		return -1;
	}
	return close(fd);
}

static int create_file(const char *path, size_t size)
{
	unsigned char page[4096];
	size_t offset;
	int fd;

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd < 0)
		return -1;
	memset(page, 0xa5, sizeof(page));
	for (offset = 0; offset < size; offset += sizeof(page)) {
		if (pwrite(fd, page, sizeof(page), offset) != sizeof(page)) {
			close(fd);
			return -1;
		}
	}
	if (fsync(fd) || posix_fadvise(fd, 0, size, POSIX_FADV_DONTNEED)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int cache_file(int fd, size_t size)
{
	unsigned char byte;
	size_t offset;

	if (posix_fadvise(fd, 0, size, POSIX_FADV_RANDOM))
		return -1;
	for (offset = 0; offset < size; offset += getpagesize())
		if (pread(fd, &byte, 1, offset) != 1)
			return -1;
	return 0;
}

static unsigned long memory_stat(const char *path, const char *key)
{
	unsigned long value;
	char name[64];
	FILE *file;

	file = fopen(path, "r");
	if (!file)
		return 0;
	while (fscanf(file, "%63s %lu", name, &value) == 2) {
		if (!strcmp(name, key)) {
			fclose(file);
			return value;
		}
	}
	fclose(file);
	return 0;
}

static int touch_ready(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

	if (fd < 0)
		return -1;
	return close(fd);
}

static bool calibration_sufficient(size_t pages, unsigned long selected,
				   unsigned long refaulted,
				   unsigned long anon)
{
	return selected >= pages / 2 && refaulted >= selected * 3 / 4 &&
	       anon >= TARGET_SIZE;
}

int main(int argc, char **argv)
{
	unsigned long refault_before, refault_after;
	unsigned long activate_before, activate_after;
	unsigned long anon, selected = 0;
	unsigned char *resident, *anon_memory;
	unsigned char byte, checksum = 0;
	char stat_path[4096];
	sigset_t signals;
	size_t pages, i;
	void *mapping;
	int target_fd, filler_fd, signal;

	if (argc != 6) {
		fprintf(stderr, "usage: %s CGROUP.PROCS TARGET FILLER READY GO\n",
			argv[0]);
		return 1;
	}
	if (join_cgroup(argv[1])) {
		perror("join cgroup");
		return 1;
	}

	anon_memory = mmap(NULL, ANON_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (anon_memory == MAP_FAILED) {
		perror("mmap anonymous");
		return 1;
	}
	for (i = 0; i < ANON_SIZE; i += getpagesize())
		anon_memory[i] = i / getpagesize() % 251 + 1;

	target_fd = create_file(argv[2], TARGET_SIZE);
	filler_fd = create_file(argv[3], FILLER_SIZE);
	if (target_fd < 0 || filler_fd < 0) {
		perror("create files");
		return 1;
	}
	if (cache_file(target_fd, TARGET_SIZE) ||
	    cache_file(filler_fd, FILLER_SIZE)) {
		perror("populate page cache");
		return 1;
	}

	sigemptyset(&signals);
	sigaddset(&signals, SIGUSR1);
	if (sigprocmask(SIG_BLOCK, &signals, NULL) || touch_ready(argv[4])) {
		perror("prepare signal");
		return 1;
	}
	if (sigwait(&signals, &signal)) {
		perror("sigwait");
		return 1;
	}

	mapping = mmap(NULL, TARGET_SIZE, PROT_READ, MAP_SHARED, target_fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap target");
		return 1;
	}
	pages = TARGET_SIZE / getpagesize();
	resident = calloc(pages, 1);
	if (!resident || mincore(mapping, TARGET_SIZE, resident)) {
		perror("mincore");
		return 1;
	}
	munmap(mapping, TARGET_SIZE);

	snprintf(stat_path, sizeof(stat_path), "%.*s/memory.stat",
		 (int)(strlen(argv[1]) - strlen("/cgroup.procs")), argv[1]);
	refault_before = memory_stat(stat_path, "workingset_refault_file");
	activate_before = memory_stat(stat_path, "workingset_activate_file");
	for (i = 0; i < pages; i++) {
		if (resident[i] & 1)
			continue;
		if (pread(target_fd, &byte, 1, i * getpagesize()) != 1) {
			perror("refault target");
			return 1;
		}
		selected++;
	}
	refault_after = memory_stat(stat_path, "workingset_refault_file");
	activate_after = memory_stat(stat_path, "workingset_activate_file");
	anon = memory_stat(stat_path, "active_anon") +
	       memory_stat(stat_path, "inactive_anon");

	for (i = 0; i < ANON_SIZE; i += getpagesize())
		checksum ^= anon_memory[i];
	printf("selected=%lu refault=%lu activate=%lu anon=%lu checksum=%u\n",
	       selected, refault_after - refault_before,
	       activate_after - activate_before, anon, checksum);

	free(resident);
	close(filler_fd);
	close(target_fd);
	if (!calibration_sufficient(pages, selected,
				    refault_after - refault_before, anon))
		return 2;
	return (activate_after - activate_before) * 100 < selected * 80;
}
