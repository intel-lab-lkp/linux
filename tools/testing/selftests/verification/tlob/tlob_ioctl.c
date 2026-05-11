// SPDX-License-Identifier: GPL-2.0
/*
 * tlob_ioctl.c - ioctl test driver and ELF utility for tlob selftests
 *
 * Usage: tlob_ioctl <subcommand> [args...]
 *
 *   not_enabled          - TRACE_START without monitor enabled -> ENODEV
 *   within_budget        - sleep within budget -> 0
 *   over_budget_running  - busy-spin past budget -> EOVERFLOW
 *   over_budget_sleeping - sleep past budget -> EOVERFLOW
 *   over_budget_waiting  - sched_yield into waiting state -> EOVERFLOW
 *   double_start         - two starts without stop -> EALREADY
 *   stop_no_start        - stop without start -> EINVAL
 *   multi_thread         - two fds: thread A within budget, thread B over
 *   bench                - TRACE_START/STOP latency (TAP output, always passes)
 *   sym_offset <binary> <symbol> - print ELF file offset of symbol
 *
 * Exit: 0 = pass, 1 = fail, 2 = skip (device not available).
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/rv.h>

static int rv_fd = -1;

static int open_rv(void)
{
	struct rv_bind_args bind = { .monitor_name = "tlob" };

	rv_fd = open("/dev/rv", O_RDWR);
	if (rv_fd < 0) {
		fprintf(stderr, "open /dev/rv: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(rv_fd, RV_IOCTL_BIND_MONITOR, &bind) < 0) {
		fprintf(stderr, "bind tlob: %s\n", strerror(errno));
		close(rv_fd);
		rv_fd = -1;
		return -1;
	}
	return 0;
}

static void busy_spin_us(unsigned long us)
{
	struct timespec start, now;
	unsigned long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (unsigned long)(now.tv_sec - start.tv_sec)
			  * 1000000000UL
			+ (unsigned long)(now.tv_nsec - start.tv_nsec);
	} while (elapsed < us * 1000UL);
}

static int trace_start(uint64_t threshold_us)
{
	struct tlob_start_args args = {
		.threshold_us = threshold_us,
	};

	return ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args);
}

static int trace_stop(void)
{
	return ioctl(rv_fd, TLOB_IOCTL_TRACE_STOP, NULL);
}

/* Synchronous TRACE_START / TRACE_STOP tests */

/* Bind to a disabled monitor must return ENODEV without crashing */
static int test_not_enabled(void)
{
	struct rv_bind_args bind = { .monitor_name = "tlob" };
	int fd;
	int ret;

	fd = open("/dev/rv", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open /dev/rv: %s\n", strerror(errno));
		return 2; /* skip */
	}

	ret = ioctl(fd, RV_IOCTL_BIND_MONITOR, &bind);
	close(fd);

	if (ret == 0) {
		fprintf(stderr, "RV_IOCTL_BIND_MONITOR: expected ENODEV, got success\n");
		return 1;
	}
	if (errno != ENODEV) {
		fprintf(stderr, "RV_IOCTL_BIND_MONITOR: expected ENODEV, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

static int test_within_budget(void)
{
	int ret;

	/* 50 ms budget */
	if (trace_start(50000) < 0) {
		fprintf(stderr, "TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	usleep(10000); /* 10 ms */
	ret = trace_stop();
	if (ret != 0) {
		fprintf(stderr, "TRACE_STOP: expected 0, got %d errno=%s\n",
			ret, strerror(errno));
		return 1;
	}
	return 0;
}

static int test_over_budget_running(void)
{
	int ret;

	/* 1 ms budget */
	if (trace_start(1000) < 0) {
		fprintf(stderr, "TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	busy_spin_us(100000); /* 100 ms */
	ret = trace_stop();
	if (ret == 0) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got 0\n");
		return 1;
	}
	if (errno != EOVERFLOW) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

static int test_over_budget_sleeping(void)
{
	int ret;

	/* 3 ms budget */
	if (trace_start(3000) < 0) {
		fprintf(stderr, "TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	usleep(50000); /* 50 ms; sleeping time counts toward budget */
	ret = trace_stop();
	if (ret == 0) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got 0\n");
		return 1;
	}
	if (errno != EOVERFLOW) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

static int test_over_budget_waiting(void)
{
	int ret;

	/* 1 us budget */
	if (trace_start(1) < 0) {
		fprintf(stderr, "TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	sched_yield(); /* running -> waiting -> running */
	busy_spin_us(10); /* 10 us >> 1 us budget; hrtimer fires during spin */
	ret = trace_stop();
	if (ret == 0) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got 0\n");
		return 1;
	}
	if (errno != EOVERFLOW) {
		fprintf(stderr, "TRACE_STOP: expected EOVERFLOW, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

/* Error-handling tests */

static int test_double_start(void)
{
	int ret;

	/* 10 s: large enough the hrtimer won't fire during the test */
	if (trace_start(10000000ULL) < 0) {
		fprintf(stderr, "first TRACE_START: %s\n", strerror(errno));
		return 1;
	}
	ret = trace_start(10000000);
	if (ret == 0) {
		fprintf(stderr, "second TRACE_START: expected EALREADY, got 0\n");
		trace_stop();
		return 1;
	}
	if (errno != EALREADY) {
		fprintf(stderr, "second TRACE_START: expected EALREADY, got %s\n",
			strerror(errno));
		trace_stop();
		return 1;
	}
	trace_stop();
	return 0;
}

static int test_stop_no_start(void)
{
	int ret;

	/* Ensure clean state: ignore error from a stale entry */
	trace_stop();

	ret = trace_stop();
	if (ret == 0) {
		fprintf(stderr, "TRACE_STOP: expected EINVAL, got 0\n");
		return 1;
	}
	if (errno != EINVAL) {
		fprintf(stderr, "TRACE_STOP: expected EINVAL, got %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

/* Two threads, each with its own fd: A within budget, B over budget. */

struct mt_thread_args {
	uint64_t      threshold_us;
	unsigned long workload_us;
	int           busy;
	int           expect_eoverflow;
	int           result;
};

static void *mt_thread_fn(void *arg)
{
	struct mt_thread_args *a = arg;
	struct tlob_start_args args = { .threshold_us = a->threshold_us };
	struct rv_bind_args bind = { .monitor_name = "tlob" };
	int fd;
	int ret;

	fd = open("/dev/rv", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "thread open /dev/rv: %s\n", strerror(errno));
		a->result = 1;
		return NULL;
	}
	if (ioctl(fd, RV_IOCTL_BIND_MONITOR, &bind) < 0) {
		fprintf(stderr, "thread bind tlob: %s\n", strerror(errno));
		close(fd);
		a->result = 1;
		return NULL;
	}

	ret = ioctl(fd, TLOB_IOCTL_TRACE_START, &args);
	if (ret < 0) {
		fprintf(stderr, "thread TRACE_START: %s\n", strerror(errno));
		close(fd);
		a->result = 1;
		return NULL;
	}

	if (a->busy)
		busy_spin_us(a->workload_us);
	else
		usleep(a->workload_us);

	ret = ioctl(fd, TLOB_IOCTL_TRACE_STOP, NULL);
	if (a->expect_eoverflow) {
		if (ret == 0 || errno != EOVERFLOW) {
			fprintf(stderr, "thread: expected EOVERFLOW, got ret=%d errno=%s\n",
				ret, strerror(errno));
			close(fd);
			a->result = 1;
			return NULL;
		}
	} else {
		if (ret != 0) {
			fprintf(stderr, "thread: expected 0, got ret=%d errno=%s\n",
				ret, strerror(errno));
			close(fd);
			a->result = 1;
			return NULL;
		}
	}
	close(fd);
	a->result = 0;
	return NULL;
}

static int test_multi_thread(void)
{
	pthread_t ta, tb;
	struct mt_thread_args a = {
		.threshold_us     = 20000,   /* 20 ms */
		.workload_us      = 5000,    /* 5 ms sleep -> within budget */
		.busy             = 0,
		.expect_eoverflow = 0,
	};
	struct mt_thread_args b = {
		.threshold_us     = 3000,    /* 3 ms */
		.workload_us      = 30000,   /* 30 ms spin -> over budget */
		.busy             = 1,
		.expect_eoverflow = 1,
	};

	pthread_create(&ta, NULL, mt_thread_fn, &a);
	pthread_create(&tb, NULL, mt_thread_fn, &b);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);

	return (a.result || b.result) ? 1 : 0;
}

/*
 * Benchmark TRACE_START, TRACE_STOP, and round-trip ioctls.
 * Output uses TAP '#' prefix; always returns 0.
 */
#define BENCH_WARMUP  32
#define BENCH_N      1000

static long long timespec_diff_ns(const struct timespec *a,
				   const struct timespec *b)
{
	return (long long)(b->tv_sec - a->tv_sec) * 1000000000LL
		+ (b->tv_nsec - a->tv_nsec);
}

static int test_bench(void)
{
	struct tlob_start_args args = {
		.threshold_us = 10000000ULL, /* 10 s */
	};
	struct timespec t0, t1;
	long long total_start_ns = 0, total_stop_ns = 0, total_rt_ns = 0;
	int i;

	/* warm up */
	for (i = 0; i < BENCH_WARMUP; i++) {
		if (ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args) == 0)
			ioctl(rv_fd, TLOB_IOCTL_TRACE_STOP, NULL);
	}

	/* start only */
	for (i = 0; i < BENCH_N; i++) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		total_start_ns += timespec_diff_ns(&t0, &t1);
		ioctl(rv_fd, TLOB_IOCTL_TRACE_STOP, NULL);
	}

	/* stop only */
	for (i = 0; i < BENCH_N; i++) {
		ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		ioctl(rv_fd, TLOB_IOCTL_TRACE_STOP, NULL);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		total_stop_ns += timespec_diff_ns(&t0, &t1);
	}

	/* round-trip */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < BENCH_N; i++) {
		ioctl(rv_fd, TLOB_IOCTL_TRACE_START, &args);
		ioctl(rv_fd, TLOB_IOCTL_TRACE_STOP, NULL);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	total_rt_ns = timespec_diff_ns(&t0, &t1);

	printf("# start ioctl only:      %lld ns/iter (N=%d, includes syscall)\n",
	       total_start_ns / BENCH_N, BENCH_N);
	printf("# stop ioctl only:       %lld ns/iter (N=%d, includes syscall)\n",
	       total_stop_ns / BENCH_N, BENCH_N);
	printf("# start+stop roundtrip:  %lld ns/iter (N=%d, includes 2 syscalls)\n",
	       total_rt_ns / BENCH_N, BENCH_N);
	return 0;
}

/*
 * Print the ELF file offset of <symname> in <binary>.  Walks .symtab
 * (falling back to .dynsym) and converts vaddr to file offset via PT_LOAD.
 * Supports 32- and 64-bit ELF.
 */
static int sym_offset(const char *binary, const char *symname)
{
	int fd;
	struct stat st;
	void *map;
	Elf64_Ehdr *ehdr;
	Elf32_Ehdr *ehdr32;
	int is64;
	uint64_t sym_vaddr = 0;
	int found = 0;
	uint64_t file_offset = 0;

	fd = open(binary, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", binary, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) < 0) {
		close(fd);
		return 1;
	}
	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}

	ehdr = (Elf64_Ehdr *)map;
	ehdr32 = (Elf32_Ehdr *)map;
	if (st.st_size < 4 ||
	    ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3) {
		fprintf(stderr, "%s: not an ELF file\n", binary);
		munmap(map, (size_t)st.st_size);
		return 1;
	}
	is64 = (ehdr->e_ident[EI_CLASS] == ELFCLASS64);

	if (is64) {
		Elf64_Shdr *shdrs = (Elf64_Shdr *)((char *)map + ehdr->e_shoff);
		Elf64_Shdr *shstrtab_hdr = &shdrs[ehdr->e_shstrndx];
		const char *shstrtab = (char *)map + shstrtab_hdr->sh_offset;
		int si;

		/* prefer .symtab; fall back to .dynsym */
		for (int pass = 0; pass < 2 && !found; pass++) {
			const char *target = pass ? ".dynsym" : ".symtab";

			for (si = 0; si < ehdr->e_shnum && !found; si++) {
				Elf64_Shdr *sh = &shdrs[si];
				const char *name = shstrtab + sh->sh_name;

				if (strcmp(name, target) != 0)
					continue;

				Elf64_Shdr *strtab_sh = &shdrs[sh->sh_link];
				const char *strtab = (char *)map + strtab_sh->sh_offset;
				Elf64_Sym *syms = (Elf64_Sym *)((char *)map + sh->sh_offset);
				uint64_t nsyms = sh->sh_size / sizeof(Elf64_Sym);
				uint64_t j;

				for (j = 0; j < nsyms; j++) {
					if (strcmp(strtab + syms[j].st_name, symname) == 0) {
						sym_vaddr = syms[j].st_value;
						found = 1;
						break;
					}
				}
			}
		}

		if (!found) {
			fprintf(stderr, "symbol '%s' not found in %s\n", symname, binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}

		/* Convert vaddr to file offset via PT_LOAD segments */
		Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)map + ehdr->e_phoff);
		int pi;

		for (pi = 0; pi < ehdr->e_phnum; pi++) {
			Elf64_Phdr *ph = &phdrs[pi];

			if (ph->p_type != PT_LOAD)
				continue;
			if (sym_vaddr >= ph->p_vaddr &&
			    sym_vaddr < ph->p_vaddr + ph->p_filesz) {
				file_offset = sym_vaddr - ph->p_vaddr + ph->p_offset;
				break;
			}
		}
	} else {
		/* 32-bit ELF */
		Elf32_Shdr *shdrs = (Elf32_Shdr *)((char *)map + ehdr32->e_shoff);
		Elf32_Shdr *shstrtab_hdr = &shdrs[ehdr32->e_shstrndx];
		const char *shstrtab = (char *)map + shstrtab_hdr->sh_offset;
		int si;
		uint32_t sym_vaddr32 = 0;

		for (int pass = 0; pass < 2 && !found; pass++) {
			const char *target = pass ? ".dynsym" : ".symtab";

			for (si = 0; si < ehdr32->e_shnum && !found; si++) {
				Elf32_Shdr *sh = &shdrs[si];
				const char *name = shstrtab + sh->sh_name;

				if (strcmp(name, target) != 0)
					continue;

				Elf32_Shdr *strtab_sh = &shdrs[sh->sh_link];
				const char *strtab = (char *)map + strtab_sh->sh_offset;
				Elf32_Sym *syms = (Elf32_Sym *)((char *)map + sh->sh_offset);
				uint32_t nsyms = sh->sh_size / sizeof(Elf32_Sym);
				uint32_t j;

				for (j = 0; j < nsyms; j++) {
					if (strcmp(strtab + syms[j].st_name, symname) == 0) {
						sym_vaddr32 = syms[j].st_value;
						found = 1;
						break;
					}
				}
			}
		}

		if (!found) {
			fprintf(stderr, "symbol '%s' not found in %s\n", symname, binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}

		Elf32_Phdr *phdrs = (Elf32_Phdr *)((char *)map + ehdr32->e_phoff);
		int pi;

		for (pi = 0; pi < ehdr32->e_phnum; pi++) {
			Elf32_Phdr *ph = &phdrs[pi];

			if (ph->p_type != PT_LOAD)
				continue;
			if (sym_vaddr32 >= ph->p_vaddr &&
			    sym_vaddr32 < ph->p_vaddr + ph->p_filesz) {
				file_offset = sym_vaddr32 - ph->p_vaddr + ph->p_offset;
				break;
			}
		}
		sym_vaddr = sym_vaddr32;
	}

	munmap(map, (size_t)st.st_size);

	if (!file_offset && sym_vaddr) {
		fprintf(stderr, "could not map vaddr 0x%lx to file offset\n",
			(unsigned long)sym_vaddr);
		return 1;
	}

	printf("0x%lx\n", (unsigned long)file_offset);
	return 0;
}

int main(int argc, char *argv[])
{
	int rc;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <subcommand> [args...]\n", argv[0]);
		return 1;
	}

	/* sym_offset does not need /dev/rv */
	if (strcmp(argv[1], "sym_offset") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s sym_offset <binary> <symbol>\n",
				argv[0]);
			return 1;
		}
		return sym_offset(argv[2], argv[3]);
	}

	/* not_enabled: monitor is disabled; bind must return ENODEV without open_rv() */
	if (strcmp(argv[1], "not_enabled") == 0)
		return test_not_enabled();

	if (open_rv() < 0)
		return 2; /* skip */

	if (strcmp(argv[1], "bench") == 0)
		rc = test_bench();
	else if (strcmp(argv[1], "within_budget") == 0)
		rc = test_within_budget();
	else if (strcmp(argv[1], "over_budget_running") == 0)
		rc = test_over_budget_running();
	else if (strcmp(argv[1], "over_budget_sleeping") == 0)
		rc = test_over_budget_sleeping();
	else if (strcmp(argv[1], "over_budget_waiting") == 0)
		rc = test_over_budget_waiting();
	else if (strcmp(argv[1], "double_start") == 0)
		rc = test_double_start();
	else if (strcmp(argv[1], "stop_no_start") == 0)
		rc = test_stop_no_start();
	else if (strcmp(argv[1], "multi_thread") == 0)
		rc = test_multi_thread();
	else {
		fprintf(stderr, "Unknown test: %s\n", argv[1]);
		rc = 1;
	}

	close(rv_fd);
	return rc;
}
