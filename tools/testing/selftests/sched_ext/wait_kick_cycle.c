// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Christian Loehle <christian.loehle@arm.com>
 */
#define _GNU_SOURCE

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#include <bpf/bpf.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <scx/common.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "scx_test.h"
#include "wait_kick_cycle.bpf.skel.h"

/*
 * Multiple workers per test CPU. Packing several runnable threads onto each
 * CPU causes frequent context switching and back-to-back enqueue() calls, which
 * maximizes the chance that all three test CPUs fire enqueue() concurrently
 * and enter the SCX_KICK_WAIT cycle simultaneously.
 */
#define WORKERS_PER_CPU	4
#define NR_TEST_CPUS	3
#define NR_WORKERS	(NR_TEST_CPUS * WORKERS_PER_CPU)

struct worker_ctx {
	pthread_t tid;
	int cpu;
	volatile bool stop;
	volatile __u64 iters;
	bool started;
};

static int pick_test_cpus(int *cpu_a, int *cpu_b, int *cpu_c)
{
	cpu_set_t mask;
	int cpus[4];
	int nr = 0;
	int cpu;

	if (sched_getaffinity(0, sizeof(mask), &mask))
		return -errno;

	for (cpu = 0; cpu < CPU_SETSIZE && nr < ARRAY_SIZE(cpus); cpu++) {
		if (!CPU_ISSET(cpu, &mask))
			continue;
		cpus[nr++] = cpu;
	}

	if (nr < 3)
		return -EOPNOTSUPP;

	/* Leave one CPU unused when possible so one CPU remains uncongested. */
	if (nr >= 4) {
		*cpu_a = cpus[1];
		*cpu_b = cpus[2];
		*cpu_c = cpus[3];
	} else {
		*cpu_a = cpus[0];
		*cpu_b = cpus[1];
		*cpu_c = cpus[2];
	}
	return 0;
}

static void *worker_fn(void *arg)
{
	struct worker_ctx *worker = arg;
	cpu_set_t mask;

	CPU_ZERO(&mask);
	CPU_SET(worker->cpu, &mask);

	if (sched_setaffinity(0, sizeof(mask), &mask))
		return (void *)(uintptr_t)errno;

	/*
	 * Tight yield loop — no sleep.  Keeping the CPU continuously busy
	 * with rapid context switches ensures enqueue() fires at the highest
	 * possible rate on each test CPU.
	 */
	while (!worker->stop) {
		sched_yield();
		worker->iters++;
	}

	return NULL;
}

static int join_worker(struct worker_ctx *worker)
{
	void *ret;
	struct timespec ts;
	int err;

	if (!worker->started)
		return 0;

	if (clock_gettime(CLOCK_REALTIME, &ts))
		return -errno;

	ts.tv_sec += 2;
	err = pthread_timedjoin_np(worker->tid, &ret, &ts);
	if (err == ETIMEDOUT)
		pthread_detach(worker->tid);
	if (err)
		return -err;

	if ((uintptr_t)ret)
		return -(int)(uintptr_t)ret;

	return 0;
}

static enum scx_test_status setup(void **ctx)
{
	struct wait_kick_cycle *skel;

	skel = wait_kick_cycle__open();
	SCX_FAIL_IF(!skel, "Failed to open skel");
	SCX_ENUM_INIT(skel);

	*ctx = skel;
	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct wait_kick_cycle *skel = ctx;
	struct worker_ctx workers[NR_WORKERS] = {};
	struct bpf_link *link = NULL;
	enum scx_test_status status = SCX_TEST_PASS;
	int test_cpus[NR_TEST_CPUS] = { -1, -1, -1 };
	int ret;
	int i;

	ret = pick_test_cpus(&test_cpus[0], &test_cpus[1], &test_cpus[2]);
	if (ret == -EOPNOTSUPP)
		return SCX_TEST_SKIP;
	if (ret) {
		SCX_ERR("Failed to pick test cpus (%d)", ret);
		return SCX_TEST_FAIL;
	}

	skel->rodata->test_cpu_a = test_cpus[0];
	skel->rodata->test_cpu_b = test_cpus[1];
	skel->rodata->test_cpu_c = test_cpus[2];

	if (wait_kick_cycle__load(skel)) {
		SCX_ERR("Failed to load skel");
		return SCX_TEST_FAIL;
	}

	link = bpf_map__attach_struct_ops(skel->maps.wait_kick_cycle_ops);
	if (!link) {
		SCX_ERR("Failed to attach scheduler");
		return SCX_TEST_FAIL;
	}

	/* WORKERS_PER_CPU threads per test CPU, all in tight yield loops. */
	for (i = 0; i < NR_WORKERS; i++)
		workers[i].cpu = test_cpus[i / WORKERS_PER_CPU];

	for (i = 0; i < NR_WORKERS; i++) {
		ret = pthread_create(&workers[i].tid, NULL, worker_fn, &workers[i]);
		if (ret) {
			SCX_ERR("Failed to create worker thread %d (%d)", i, ret);
			status = SCX_TEST_FAIL;
			goto out;
		}
		workers[i].started = true;
	}

	sleep(3);

	if (skel->data->uei.kind != EXIT_KIND(SCX_EXIT_NONE)) {
		SCX_ERR("Scheduler exited unexpectedly (kind=%llu code=%lld)",
			(unsigned long long)skel->data->uei.kind,
			(long long)skel->data->uei.exit_code);
		status = SCX_TEST_FAIL;
	}

out:
	for (i = 0; i < NR_WORKERS; i++)
		workers[i].stop = true;

	for (i = 0; i < NR_WORKERS; i++) {
		ret = join_worker(&workers[i]);
		if (ret && status == SCX_TEST_PASS) {
			SCX_ERR("Failed to join worker thread %d (%d)", i, ret);
			status = SCX_TEST_FAIL;
		}
	}

	if (link)
		bpf_link__destroy(link);

	return status;
}

static void cleanup(void *ctx)
{
	struct wait_kick_cycle *skel = ctx;

	wait_kick_cycle__destroy(skel);
}

struct scx_test wait_kick_cycle = {
	.name = "wait_kick_cycle",
	.description = "Verify SCX_KICK_WAIT forward progress under a 3-CPU wait cycle",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&wait_kick_cycle)
