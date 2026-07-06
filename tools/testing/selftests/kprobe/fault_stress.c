// SPDX-License-Identifier: GPL-2.0
/*
 * fault_stress.c - Userspace program to trigger file-backed page faults
 *
 * This program creates a file and repeatedly maps/unmaps it while
 * accessing memory, triggering file-backed page faults. It's designed
 * to work with kprobe-folio-stress.c to stress kprobe handling.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FILE_SIZE   (256UL * 1024 * 1024)
#define NR_THREADS  8

static void deep_call(int n)
{
	volatile char buf[4096];

	memset((void *)buf, n, sizeof(buf));

	if (n > 0)
		deep_call(n - 1);
	else
		sched_yield();
}

static void *worker(void *arg)
{
	const char *path = arg;
	int fd;
	char *map;
	unsigned long i;
	volatile unsigned long sum = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return NULL;
	}

	map = mmap(NULL, FILE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return NULL;
	}

	for (;;) {
		/*
		 * Drop the pages backing this mapping from the current
		 * process. Subsequent accesses are more likely to trigger
		 * file-backed page faults again.
		 */
		madvise(map, FILE_SIZE, MADV_DONTNEED);

		for (i = 0; i < FILE_SIZE; i += 4096 * 17) {
			sum += map[i];
			deep_call(64);
		}
	}

	munmap(map, FILE_SIZE);
	close(fd);
	return NULL;
}

int main(void)
{
	pthread_t th[NR_THREADS];
	const char *path = "/tmp/fault_stress_file";
	int fd;
	int i;

	fd = open(path, O_CREAT | O_RDWR, 0644);
	if (fd < 0) {
		perror("open file");
		return 1;
	}

	if (ftruncate(fd, FILE_SIZE) < 0) {
		perror("ftruncate");
		close(fd);
		return 1;
	}

	close(fd);

	for (i = 0; i < NR_THREADS; i++)
		pthread_create(&th[i], NULL, worker, (void *)path);

	for (i = 0; i < NR_THREADS; i++)
		pthread_join(th[i], NULL);

	return 0;
}
