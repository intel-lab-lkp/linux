// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userfaultfd performance tests.
 *
 *  Copyright (C) 2023  Red Hat, Inc.
 */

#include "uffd-common.h"

#ifdef __NR_userfaultfd

#define  DEF_MEM_SIZE_MB  (512)
#define  MB(x)  ((x) * 1024 * 1024)
#define  DEF_N_TESTS  5

static volatile bool perf_test_started;
static unsigned int n_uffd_threads, n_worker_threads;
static uint64_t nr_pages_per_worker;
static unsigned long n_tests = DEF_N_TESTS;

static void setup_env(unsigned long mem_size_mb)
{
	/* Test private anon only for now */
	map_shared = false;
	uffd_test_ops = &anon_uffd_test_ops;
	page_size = psize();
	nr_cpus = n_uffd_threads;
	nr_pages = MB(mem_size_mb) / page_size;
	nr_pages_per_worker = nr_pages / n_worker_threads;
	if (nr_pages_per_worker == 0)
		err("each worker should at least own one page");
}

void *worker_fn(void *opaque)
{
	unsigned long i = (unsigned long) opaque;
	unsigned long page_nr, start_nr, end_nr;
	int v = 0;

	start_nr = i * nr_pages_per_worker;
	end_nr = (i + 1) * nr_pages_per_worker;

	while (!perf_test_started);

	for (page_nr = start_nr; page_nr < end_nr; page_nr++)
		v += *(volatile int *)(area_dst + page_nr * page_size);

	return NULL;
}

static uint64_t run_perf(uint64_t mem_size_mb, bool poll)
{
	pthread_t worker_threads[n_worker_threads];
	pthread_t uffd_threads[n_uffd_threads];
	const char *errmsg = NULL;
	struct uffd_args *args;
	uint64_t start, end;
	int i, ret;

	if (uffd_test_ctx_init(0, &errmsg))
		err("%s", errmsg);

	/*
	 * By default, uffd is opened with NONBLOCK mode; use block mode
	 * when test read()
	 */
	if (!poll) {
		int flags = fcntl(uffd, F_GETFL);

		if (flags < 0)
			err("fcntl(F_GETFL) failed");

		if (flags & O_NONBLOCK)
			flags &= ~O_NONBLOCK;

		if (fcntl(uffd, F_SETFL, flags))
			err("fcntl(F_SETFL) failed");
	}

	ret = uffd_register(uffd, area_dst, MB(mem_size_mb),
			    true, false, false);
	if (ret)
		err("uffd_register() failed");

	args = calloc(nr_cpus, sizeof(struct uffd_args));
	if (!args)
		err("calloc()");

	for (i = 0; i < n_uffd_threads; i++) {
		args[i].cpu = i;
		uffd_fault_thread_create(&uffd_threads[i], NULL,
					 &args[i], poll);
	}

	for (i = 0; i < n_worker_threads; i++) {
		if (pthread_create(&worker_threads[i], NULL,
				   worker_fn, (void *)(uintptr_t)i))
			err("create uffd threads");
	}

	start = get_usec();
	perf_test_started = true;
	for (i = 0; i < n_worker_threads; i++)
		pthread_join(worker_threads[i], NULL);
	end = get_usec();

	for (i = 0; i < n_uffd_threads; i++) {
		struct uffd_args *p = &args[i];

		uffd_fault_thread_join(uffd_threads[i], i, poll);

		assert(p->wp_faults == 0 && p->minor_faults == 0);
	}

	free(args);

	ret = uffd_unregister(uffd, area_dst, MB(mem_size_mb));
	if (ret)
		err("uffd_unregister() failed");

	return end - start;
}

static void usage(const char *prog)
{
	printf("usage: %s <options>\n", prog);
	puts("");
	printf("  -m: size of memory to test (in MB, default: %u)\n",
	       DEF_MEM_SIZE_MB);
	puts("  -p: use poll() (the default)");
	puts("  -r: use read()");
	printf("  -t: test rounds (default: %u)\n", DEF_N_TESTS);
	puts("  -u: number of uffd threads (default: n_cpus)");
	puts("  -w: number of worker threads (default: n_cpus)");
	puts("");
	exit(KSFT_FAIL);
}

int main(int argc, char *argv[])
{
	unsigned long mem_size_mb = DEF_MEM_SIZE_MB;
	uint64_t result, sum = 0;
	bool use_poll = true;
	int opt, count;

	n_uffd_threads = n_worker_threads = sysconf(_SC_NPROCESSORS_ONLN);

	while ((opt = getopt(argc, argv, "hm:prt:u:w:")) != -1) {
		switch (opt) {
		case 'm':
			mem_size_mb = strtoul(optarg, NULL, 10);
			break;
		case 'p':
			use_poll = true;
			break;
		case 'r':
			use_poll = false;
			break;
		case 't':
			n_tests = strtoul(optarg, NULL, 10);
			break;
		case 'u':
			n_uffd_threads = strtoul(optarg, NULL, 10);
			break;
		case 'w':
			n_worker_threads = strtoul(optarg, NULL, 10);
			break;
		case 'h':
		default:
			/* Unknown */
			usage(argv[0]);
			break;
		}
	}

	setup_env(mem_size_mb);

	printf("Message mode: \t\t%s\n", use_poll ? "poll" : "read");
	printf("Mem size: \t\t%lu (MB)\n", mem_size_mb);
	printf("Uffd threads: \t\t%u\n", n_uffd_threads);
	printf("Worker threads: \t%u\n", n_worker_threads);
	printf("Test rounds: \t\t%lu\n", n_tests);
	printf("Time used (us): \t");

	for (count = 0; count < n_tests; count++) {
		result = run_perf(mem_size_mb, use_poll);
		sum += result;
		printf("%" PRIu64 ", ", result);
		fflush(stdout);
	}
	printf("\b\b \n");
	printf("Average (us): \t\t%"PRIu64"\n", sum / n_tests);

	return KSFT_PASS;
}

#else /* __NR_userfaultfd */

#warning "missing __NR_userfaultfd definition"

int main(void)
{
	printf("Skipping %s (missing __NR_userfaultfd)\n", __file__);
	return KSFT_SKIP;
}

#endif /* __NR_userfaultfd */
