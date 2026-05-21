// SPDX-License-Identifier: GPL-2.0
/*
 * Microbenchmark for get_user_pages (GUP) kernel interfaces.
 *
 * Exercises GUP_FAST_BENCHMARK, PIN_FAST_BENCHMARK, and
 * PIN_LONGTERM_BENCHMARK via the CONFIG_GUP_TEST debugfs interface.
 *
 * Example use:
 *   # Run the full matrix (all commands, access modes, page counts):
 *   ./gup_bench
 *
 *   # Single run: pin_user_pages_fast, 512 pages, write access, hugetlb:
 *   ./gup_bench -a -n 512 -w -H
 *
 * Requires CONFIG_GUP_TEST=y and debugfs mounted at /sys/kernel/debug.
 * Must be run as root.
 */

#define __SANE_USERSPACE_TYPES__ // Use ll64
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <mm/gup_test.h>
#include <mm/hugepage_settings.h>

#define MB (1UL << 20)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

/* Just the flags we need, copied from the kernel internals. */
#define FOLL_WRITE	0x01	/* check pte is writable */

#define GUP_TEST_FILE "/sys/kernel/debug/gup_test"

static unsigned int psize(void)
{
	static unsigned int __page_size;

	if (!__page_size)
		__page_size = sysconf(_SC_PAGESIZE);
	return __page_size;
}

static unsigned long cmd;
static const char *bench_label;
static int gup_fd, repeats = 1;
static unsigned long size = 128 * MB;
static atomic_int bench_error;
/* Serialize prints */
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

static const unsigned long bench_cmds[] = {
	GUP_FAST_BENCHMARK,
	PIN_FAST_BENCHMARK,
	PIN_LONGTERM_BENCHMARK,
};
static const int bench_thp_modes[] = { 1, 0 };	/* on, off */
static const int bench_nr_pages_list[] = { 1, 512, 123, -1 };

static const char *cmd_to_str(unsigned long cmd)
{
	switch (cmd) {
	case GUP_FAST_BENCHMARK:
		return "GUP_FAST_BENCHMARK";
	case PIN_FAST_BENCHMARK:
		return "PIN_FAST_BENCHMARK";
	case PIN_LONGTERM_BENCHMARK:
		return "PIN_LONGTERM_BENCHMARK";
	}
	return "Unknown command";
}

struct bench_run {
	unsigned long cmd;
	int thp;		/* -1: default, 0: off, 1: on */
	bool hugetlb;
	bool write;
	bool shared;
	int nr_pages;		/* -1 means all pages (size / psize()) */
	unsigned long size;
	char *file;
	int nthreads;
	unsigned int gup_flags;
};

void *gup_thread(void *data)
{
	struct gup_test gup = *(struct gup_test *)data;
	int i, status;

	for (i = 0; i < repeats; i++) {
		gup.size = size;
		status = ioctl(gup_fd, cmd, &gup);
		if (status) {
			bench_error = 1;
			break;
		}

		pthread_mutex_lock(&print_mutex);
		printf("%s time: get:%lld put:%lld us",
			    bench_label, gup.get_delta_usec,
				    gup.put_delta_usec);
		if (gup.size != size)
			printf(", truncated (size: %lld)", gup.size);
		printf("\n");
		pthread_mutex_unlock(&print_mutex);
	}

	return NULL;
}

static int run_bench(struct bench_run *run)
{
	struct gup_test gup = { 0 };
	int zero_fd, i, ret, started_threads = 0;
	int flags = MAP_PRIVATE;
	pthread_t *tid;
	char label[128];
	char *p;

	/* Set globals consumed by gup_thread */
	cmd = run->cmd;
	size = run->size;
	bench_error = 0;

	if (run->hugetlb) {
		unsigned long hp_size = default_huge_page_size();

		if (!hp_size) {
			fprintf(stderr, "Could not determine huge page size\n");
			return 1;
		}
		size = (size + hp_size - 1) & ~(hp_size - 1);
		if (!hugetlb_setup_default(size / hp_size)) {
			fprintf(stderr, "Not enough huge pages\n");
			return 1;
		}
		flags |= (MAP_HUGETLB | MAP_ANONYMOUS);
	}

	if (run->shared) {
		flags &= ~MAP_PRIVATE;
		flags |= MAP_SHARED;
	}

	gup.nr_pages_per_call = run->nr_pages < 0 ? size / psize() :
		(unsigned long)run->nr_pages;

	gup.gup_flags = run->gup_flags;
	if (run->write)
		gup.gup_flags |= FOLL_WRITE;

	snprintf(label, sizeof(label), "%s (nr_pages=%-4u %s %s %s %s)",
		 cmd_to_str(run->cmd),
		 gup.nr_pages_per_call,
		 run->write  ? "write"   : "read",
		 run->shared ? "shared"  : "private",
		 run->hugetlb ? "hugetlb=on" : "hugetlb=off",
		 run->hugetlb ? "thp=off" :
		 (run->thp == 1 ? "thp=on" :
		 (run->thp == 0 ? "thp=off" : "thp=default")));
	bench_label = label;

	zero_fd = open(run->file, O_RDWR);
	if (zero_fd < 0) {
		fprintf(stderr, "Unable to open %s: %s\n", run->file, strerror(errno));
		return 1;
	}

	p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, zero_fd, 0);
	close(zero_fd);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}
	gup.addr = (unsigned long)p;

	if (run->thp == 1)
		madvise(p, size, MADV_HUGEPAGE);
	else if (run->thp == 0)
		madvise(p, size, MADV_NOHUGEPAGE);

	/* Fault them in here, from user space. */
	for (; (unsigned long)p < gup.addr + size; p += psize())
		p[0] = 0;

	tid = malloc(sizeof(pthread_t) * run->nthreads);
	if (!tid) {
		fprintf(stderr, "Failed to allocate %d threads: %s\n",
			run->nthreads, strerror(errno));
		munmap((void *)gup.addr, size);
		return 1;
	}

	for (i = 0; i < run->nthreads; i++) {
		ret = pthread_create(&tid[i], NULL, gup_thread, &gup);
		if (ret) {
			fprintf(stderr, "pthread_create failed: %s\n", strerror(ret));
			bench_error = 1;
			break;
		}
		started_threads++;
	}
	for (i = 0; i < started_threads; i++) {
		ret = pthread_join(tid[i], NULL);
		if (ret) {
			fprintf(stderr, "pthread_join failed: %s\n", strerror(ret));
			bench_error = 1;
		}
	}

	free(tid);
	munmap((void *)gup.addr, size);

	return bench_error ? 1 : 0;
}

static int run_matrix(void)
{
	unsigned int c, t, w, s, n;
	int ret = 0;

	for (c = 0; c < ARRAY_SIZE(bench_cmds); c++) {
		for (w = 0; w <= 1; w++) {
			for (s = 0; s <= 1; s++) {
				for (t = 0; t < ARRAY_SIZE(bench_thp_modes); t++) {
					for (n = 0; n < ARRAY_SIZE(bench_nr_pages_list); n++) {
						struct bench_run run = {
							.cmd	  = bench_cmds[c],
							.thp	  = bench_thp_modes[t],
							.hugetlb  = false,
							.write	  = w,
							.shared	  = s,
							.nr_pages = bench_nr_pages_list[n],
							.size	  = 128 * MB,
							.file	  = "/dev/zero",
							.nthreads = 1,
						};
						ret |= run_bench(&run);
					}
				}
				/* hugetlb: 256M to match run_gup_matrix() in run_vmtests.sh */
				for (n = 0; n < ARRAY_SIZE(bench_nr_pages_list); n++) {
					struct bench_run run = {
						.cmd	  = bench_cmds[c],
						.thp	  = -1,
						.hugetlb  = true,
						.write	  = w,
						.shared	  = s,
						.nr_pages = bench_nr_pages_list[n],
						.size	  = 256 * MB,
						.file	  = "/dev/zero",
						.nthreads = 1,
					};
					ret |= run_bench(&run);
				}
			}
		}
	}
	return ret;
}

int main(int argc, char **argv)
{
	struct bench_run run = {
		.cmd	  = GUP_FAST_BENCHMARK,
		.thp	  = -1,
		.hugetlb  = false,
		.write	  = true,
		.shared	  = false,
		.nr_pages = 1,
		.size	  = 128 * MB,
		.file	  = "/dev/zero",
		.nthreads = 1,
	};
	int opt, result;

	while ((opt = getopt(argc, argv, "m:r:n:F:f:aj:tTLuwWSH")) != -1) {
		switch (opt) {

		/* Command selection */
		case 'u':
			run.cmd = GUP_FAST_BENCHMARK;
			break;
		case 'a':
			run.cmd = PIN_FAST_BENCHMARK;
			break;
		case 'L':
			run.cmd = PIN_LONGTERM_BENCHMARK;
			break;

		/* Memory type */
		case 'H':
			run.hugetlb = true;
			break;
		case 't':
			run.thp = 1;
			break;
		case 'T':
			run.thp = 0;
			break;

		/* Access mode */
		case 'w':
			run.write = true;
			break;
		case 'W':
			run.write = false;
			break;
		case 'S':
			run.shared = true;
			break;

		/* Mapping */
		case 'f':
			run.file = optarg;
			break;

		/* Sizing and iteration */
		case 'm':
			run.size = atoi(optarg) * MB;
			break;
		case 'n':
			run.nr_pages = atoi(optarg);
			break;
		case 'r':
			repeats = atoi(optarg);
			break;
		case 'j': {
			char *end;
			long val;

			errno = 0;
			val = strtol(optarg, &end, 10);
			if (errno || end == optarg || *end != '\0' || val < 1 ||
			    val > INT_MAX ||
			    (size_t)val > SIZE_MAX / sizeof(pthread_t)) {
				fprintf(stderr, "Invalid thread count '%s'\n", optarg);
				exit(1);
			}
			run.nthreads = val;
			break;
		}

		/* Advanced */
		case 'F':
			/* strtol, so you can pass flags in hex form */
			run.gup_flags = strtol(optarg, 0, 0);
			break;

		default:
			fprintf(stderr, "Wrong argument\n");
			exit(1);
		}
	}

	gup_fd = open(GUP_TEST_FILE, O_RDWR);
	if (gup_fd == -1) {
		if (errno == EACCES) {
			fprintf(stderr, "Please run as root\n");
		} else if (errno == ENOENT) {
			if (opendir("/sys/kernel/debug") == NULL)
				fprintf(stderr, "Mount debugfs at /sys/kernel/debug\n");
			else
				fprintf(stderr, "Check CONFIG_GUP_TEST in kernel config\n");
		} else {
			fprintf(stderr, "Failed to open %s: %s\n", GUP_TEST_FILE,
				strerror(errno));
		}
		exit(1);
	}

	result = (argc == 1) ? run_matrix() : run_bench(&run);
	close(gup_fd);
	return result;
}
