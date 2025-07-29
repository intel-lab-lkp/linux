// SPDX-License-Identifier: GPL-2.0
/*
 * Synchronization benchmark.
 *
 * 2025  Yuzhuo Jing <yuzhuo@google.com>
 */
#include <bits/time.h>
#include <err.h>
#include <inttypes.h>
#include <perf/cpumap.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <subcmd/parse-options.h>
#include <sys/cdefs.h>

#include "bench.h"

#include "include/qspinlock.h"

#define NS 1000000000ull
#define CACHELINE_SIZE 64

static unsigned int nthreads;
static unsigned long nspins = 10000ul;

struct barrier_t;

typedef void(*lock_fn)(void *);

/*
 * Lock operation definition to support multiple implmentations of locks.
 *
 * The lock and unlock functions only take one variable, the data pointer.
 */
struct lock_ops {
	lock_fn lock;
	lock_fn unlock;
	void *data;
};

struct worker {
	pthread_t thd;
	unsigned int tid;
	struct lock_ops *ops;
	struct barrier_t *barrier;
	u64 runtime;		// in nanoseconds
};

static const struct option options[] = {
	OPT_UINTEGER('t',	"threads",	&nthreads,
		"Specify number of threads (default: number of CPUs)."),
	OPT_ULONG('n',		"spins",	&nspins,
		"Number of lock acquire operations per thread (default: 10,000 times)."),
	OPT_END()
};

static const char *const bench_sync_usage[] = {
	"perf bench sync qspinlock <options>",
	NULL
};

/*
 * A atomic-based barrier.  Expect to have lower latency than pthread barrier
 * that sleeps the thread.
 */
struct barrier_t {
	unsigned int count __aligned(CACHELINE_SIZE);
};

/*
 * A atomic-based barrier.  Expect to have lower latency than pthread barrier
 * that sleeps the thread.
 */
__always_inline void wait_barrier(struct barrier_t *b)
{
	if (__atomic_sub_fetch(&b->count, 1, __ATOMIC_RELAXED) == 0)
		return;
	while (__atomic_load_n(&b->count, __ATOMIC_RELAXED))
		;
}

static int bench_sync_lock_generic(struct lock_ops *ops, int argc, const char **argv);

/*
 * Benchmark of linux kernel queued spinlock in user land.
 */
int bench_sync_qspinlock(int argc, const char **argv)
{
	struct qspinlock lock = __ARCH_SPIN_LOCK_UNLOCKED;
	struct lock_ops ops = {
		.lock = (lock_fn)queued_spin_lock,
		.unlock = (lock_fn)queued_spin_unlock,
		.data = &lock,
	};
	return bench_sync_lock_generic(&ops, argc, argv);
}

/*
 * A busy loop to acquire and release the given lock N times.
 */
static void lock_loop(const struct lock_ops *ops, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; ++i) {
		ops->lock(ops->data);
		ops->unlock(ops->data);
	}
}

/*
 * Thread worker function.  Runs lock loop for N/5 times before and after
 * the main timed loop.
 */
static void *sync_workerfn(void *args)
{
	struct worker *worker = (struct worker *)args;
	struct timespec starttime, endtime;

	set_this_cpu_id(worker->tid);

	/* Barrier to let all threads start together */
	wait_barrier(worker->barrier);

	/* Warmup loop (not counted) to keep the below loop contended. */
	lock_loop(worker->ops, nspins / 5);

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &starttime);
	lock_loop(worker->ops, nspins);
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &endtime);

	/* Tail loop (not counted) to keep the above loop contended. */
	lock_loop(worker->ops, nspins / 5);

	worker->runtime = (endtime.tv_sec - starttime.tv_sec) * NS
		+ endtime.tv_nsec - starttime.tv_nsec;

	return NULL;
}

/*
 * Generic lock synchronization benchmark function.  Sets up threads and
 * thread affinities.
 */
static int bench_sync_lock_generic(struct lock_ops *ops, int argc, const char **argv)
{
	struct perf_cpu_map *online_cpus;
	unsigned int online_cpus_nr;
	struct worker *workers;
	u64 totaltime = 0, total_spins, avg_ns, avg_ns_dot;
	struct barrier_t barrier;
	cpu_set_t *cpuset;
	size_t cpuset_size;

	argc = parse_options(argc, argv, options, bench_sync_usage, 0);
	if (argc) {
		usage_with_options(bench_sync_usage, options);
		exit(EXIT_FAILURE);
	}

	/* CPU count setup. */
	online_cpus = perf_cpu_map__new_online_cpus();
	if (!online_cpus)
		err(EXIT_FAILURE, "No online CPUs available");
	online_cpus_nr = perf_cpu_map__nr(online_cpus);

	if (!nthreads) /* default to the number of CPUs */
		nthreads = online_cpus_nr;

	workers = calloc(nthreads, sizeof(*workers));
	if (!workers)
		err(EXIT_FAILURE, "calloc");

	barrier.count = nthreads;

	printf("Running with %u threads.\n", nthreads);

	cpuset = CPU_ALLOC(online_cpus_nr);
	if (!cpuset)
		err(EXIT_FAILURE, "Cannot allocate cpuset.");
	cpuset_size = CPU_ALLOC_SIZE(online_cpus_nr);

	/* Create worker data structures, set CPU affinity, and create   */
	for (unsigned int i = 0; i < nthreads; ++i) {
		pthread_attr_t thread_attr;
		int ret;

		/* Basic worker thread information */
		workers[i].tid = i;
		workers[i].barrier = &barrier;
		workers[i].ops = ops;

		/* Set CPU affinity */
		pthread_attr_init(&thread_attr);
		CPU_ZERO_S(cpuset_size, cpuset);
		CPU_SET_S(perf_cpu_map__cpu(online_cpus, i % online_cpus_nr).cpu,
			cpuset_size, cpuset);

		if (pthread_attr_setaffinity_np(&thread_attr, cpuset_size, cpuset))
			err(EXIT_FAILURE, "Pthread set affinity failed");

		/* Create and block thread */
		ret = pthread_create(&workers[i].thd, &thread_attr, sync_workerfn, &workers[i]);
		if (ret != 0)
			err(EXIT_FAILURE, "Error creating thread: %s", strerror(ret));

		pthread_attr_destroy(&thread_attr);
	}

	CPU_FREE(cpuset);

	for (unsigned int i = 0; i < nthreads; ++i) {
		int ret = pthread_join(workers[i].thd, NULL);

		if (ret)
			err(EXIT_FAILURE, "pthread_join");
	}

	/* Calculate overall average latency. */
	for (unsigned int i = 0; i < nthreads; ++i)
		totaltime += workers[i].runtime;

	total_spins = (u64)nthreads * nspins;
	avg_ns = totaltime / total_spins;
	avg_ns_dot = (totaltime % total_spins) * 10000 / total_spins;

	printf("Lock-unlock latency of %u threads: %"PRIu64".%"PRIu64" ns.\n",
			nthreads, avg_ns, avg_ns_dot);

	free(workers);

	return 0;
}
