// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that ops.dequeue() is called for tasks leaving BPF custody through
 * an SCX-internal cross-CPU migration (move_remote_task_to_local_dsq()).
 *
 * Copyright (c) 2026 Google LLC.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sched.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/prctl.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include "scx_test.h"
#include "dequeue_remote.bpf.skel.h"

#define MAX_WORKERS	64
#define RUN_MS		2000

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int get_core_cookie(pid_t tid, unsigned long *cookie)
{
	return prctl(PR_SCHED_CORE, PR_SCHED_CORE_GET, tid,
		     PR_SCHED_CORE_SCOPE_THREAD, (unsigned long)cookie);
}

/*
 * Core scheduling is in use iff some task has a core scheduling cookie. If
 * none does now, assume that none will get one for the duration of the test.
 */
static bool core_sched_in_use(void)
{
	unsigned long cookie;
	struct dirent *pde, *tde;
	bool in_use = false;
	DIR *proc, *task;
	char path[sizeof("/proc//task") + sizeof(pde->d_name)];

	/* fails with EINVAL if !CONFIG_SCHED_CORE and ENODEV if SMT is off */
	if (get_core_cookie(0, &cookie))
		return false;

	proc = opendir("/proc");
	if (!proc)
		return false;

	while (!in_use && (pde = readdir(proc))) {
		if (!isdigit(pde->d_name[0]))
			continue;

		snprintf(path, sizeof(path), "/proc/%s/task", pde->d_name);
		task = opendir(path);
		if (!task)
			continue;

		/* errors, e.g. ESRCH for exited tasks, are ignored */
		while (!in_use && (tde = readdir(task))) {
			if (isdigit(tde->d_name[0]) &&
			    !get_core_cookie(atoi(tde->d_name), &cookie))
				in_use = cookie;
		}

		closedir(task);
	}

	closedir(proc);
	return in_use;
}

/* Mix of short bursts and sleeps to generate lots of enqueues and wakeups */
static void worker_fn(int id)
{
	long long end = now_ms() + RUN_MS;
	volatile unsigned long sum = 0;

	while (now_ms() < end) {
		unsigned long j;

		for (j = 0; j < 20000 + id * 1000; j++)
			sum += j;
		if (id & 1)
			usleep(100);
		else
			sched_yield();
	}

	exit(0);
}

static enum scx_test_status run_scenario(struct dequeue_remote *skel,
					 bool use_move_to_local,
					 const char *name)
{
	struct bpf_link *link;
	pid_t pids[MAX_WORKERS];
	int nr_workers, i, status;
	u64 enq, deq, dsp_deq, chg_deq, remote_dsp, remote_run;

	nr_workers = 2 * sysconf(_SC_NPROCESSORS_ONLN);
	if (nr_workers < 4)
		nr_workers = 4;
	if (nr_workers > MAX_WORKERS)
		nr_workers = MAX_WORKERS;

	skel->bss->test_use_move_to_local = use_move_to_local;
	enq = skel->bss->enqueue_cnt;
	deq = skel->bss->dequeue_cnt;
	dsp_deq = skel->bss->dispatch_dequeue_cnt;
	chg_deq = skel->bss->change_dequeue_cnt;
	remote_dsp = skel->bss->remote_dispatch_cnt;
	remote_run = skel->bss->remote_running_cnt;

	link = bpf_map__attach_struct_ops(skel->maps.dequeue_remote_ops);
	SCX_FAIL_IF(!link, "Failed to attach struct_ops for %s", name);

	fflush(stdout);
	fflush(stderr);

	for (i = 0; i < nr_workers; i++) {
		pids[i] = fork();
		SCX_FAIL_IF(pids[i] < 0, "Failed to fork worker %d", i);
		if (pids[i] == 0)
			worker_fn(i);
	}

	for (i = 0; i < nr_workers; i++) {
		SCX_FAIL_IF(waitpid(pids[i], &status, 0) != pids[i],
			    "Failed to wait for worker %d", i);
		SCX_FAIL_IF(status != 0, "Worker %d exited with status %d", i, status);
	}

	bpf_link__destroy(link);

	enq = skel->bss->enqueue_cnt - enq;
	deq = skel->bss->dequeue_cnt - deq;
	dsp_deq = skel->bss->dispatch_dequeue_cnt - dsp_deq;
	chg_deq = skel->bss->change_dequeue_cnt - chg_deq;
	remote_dsp = skel->bss->remote_dispatch_cnt - remote_dsp;
	remote_run = skel->bss->remote_running_cnt - remote_run;

	printf("%s:\n", name);
	printf("  workers: %d\n", nr_workers);
	printf("  enqueues: %lu\n", (unsigned long)enq);
	printf("  dequeues: %lu (dispatch: %lu, property_change: %lu)\n",
	       (unsigned long)deq, (unsigned long)dsp_deq,
	       (unsigned long)chg_deq);
	if (!use_move_to_local)
		printf("  remote SCX_DSQ_LOCAL_ON dispatches: %lu\n",
		       (unsigned long)remote_dsp);
	printf("  ran on a CPU other than the enqueue CPU: %lu\n",
	       (unsigned long)remote_run);
	printf("  ran without ops.dequeue(): %lu\n",
	       (unsigned long)skel->bss->missed_dequeue_cnt);
	printf("  late SCX_DEQ_CORE_SCHED_EXEC dequeues: %lu\n",
	       (unsigned long)skel->bss->core_sched_exec_dequeue_cnt);

	if (skel->data->uei.kind != EXIT_KIND(SCX_EXIT_UNREG))
		SCX_ERR("Scheduler exited with kind=%lld: %s",
			(long long)skel->data->uei.kind, skel->data->uei.msg);
	SCX_EQ(skel->data->uei.kind, EXIT_KIND(SCX_EXIT_UNREG));

	/* the test is meaningless if no task was moved across CPUs */
	SCX_GT(remote_run, 0);
	SCX_EQ(enq, deq);

	return SCX_TEST_PASS;
}

static enum scx_test_status setup(void **ctx)
{
	struct dequeue_remote *skel;

	if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
		fprintf(stderr, "Skipping: requires at least 2 CPUs\n");
		return SCX_TEST_SKIP;
	}

	/*
	 * Core scheduling can legitimately pick tasks straight out of BPF
	 * custody, which the test would flag as late
	 * %SCX_DEQ_CORE_SCHED_EXEC dequeues.
	 */
	if (core_sched_in_use()) {
		fprintf(stderr, "Skipping: core scheduling is in use\n");
		return SCX_TEST_SKIP;
	}

	skel = SCX_OPS_OPEN(dequeue_remote_ops, dequeue_remote);
	SCX_OPS_LOAD(skel, dequeue_remote_ops, dequeue_remote, uei);

	*ctx = skel;

	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct dequeue_remote *skel = ctx;
	enum scx_test_status status;

	status = run_scenario(skel, false,
			      "BPF queue -> SCX_DSQ_LOCAL_ON | cpu");
	if (status != SCX_TEST_PASS)
		return status;

	status = run_scenario(skel, true,
			      "user DSQ -> scx_bpf_dsq_move_to_local()");
	if (status != SCX_TEST_PASS)
		return status;

	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	struct dequeue_remote *skel = ctx;

	dequeue_remote__destroy(skel);
}

struct scx_test dequeue_remote_test = {
	.name = "dequeue_remote",
	.description = "Verify ops.dequeue() on SCX-internal cross-CPU migrations",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};

REGISTER_SCX_TEST(&dequeue_remote_test)
