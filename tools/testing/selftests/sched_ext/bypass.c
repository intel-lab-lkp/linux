// SPDX-License-Identifier: GPL-2.0
/*
 * Verify the sched_ext bypass mechanism: spawn worker tasks and ensure
 * they run to completion while a BPF scheduler is active.
 *
 * The bypass mechanism (activated on scheduler unregistration) must
 * guarantee forward progress. This test verifies that worker tasks
 * complete successfully when the scheduler is detached.
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/wait.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_test.h"
#include "bypass.bpf.skel.h"

#define NUM_BYPASS_WORKERS 4

static void worker_fn(void)
{
	volatile int sum = 0;
	int i;

	/*
	 * Do enough work to still be running when bpf_link__destroy()
	 * is called, ensuring tasks are active during bypass mode.
	 */
	for (i = 0; i < 10000000; i++)
		sum += i;
}

static enum scx_test_status setup(void **ctx)
{
	struct bypass *skel;

	skel = bypass__open();
	SCX_FAIL_IF(!skel, "Failed to open bypass skel");
	SCX_ENUM_INIT(skel);
	SCX_FAIL_IF(bypass__load(skel), "Failed to load bypass skel");

	*ctx = skel;
	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *ctx)
{
	struct bypass *skel = ctx;
	struct bpf_link *link;
	pid_t pids[NUM_BYPASS_WORKERS];
	int i, status;

	link = bpf_map__attach_struct_ops(skel->maps.bypass_ops);
	SCX_FAIL_IF(!link, "Failed to attach bypass scheduler");

	/*
	 * Spawn worker processes. These must complete successfully
	 * even as the scheduler is active and then detached (which
	 * triggers bypass mode).
	 */
	for (i = 0; i < NUM_BYPASS_WORKERS; i++) {
		pids[i] = fork();
		SCX_FAIL_IF(pids[i] < 0, "fork() failed for worker %d", i);

		if (pids[i] == 0) {
			worker_fn();
			_exit(0);
		}
	}

	/*
	 * Detach the scheduler while workers are still running. This
	 * triggers bypass mode, which must guarantee forward progress
	 * for all active tasks.
	 */
	bpf_link__destroy(link);

	/* Workers must complete successfully under bypass mode */
	for (i = 0; i < NUM_BYPASS_WORKERS; i++) {
		SCX_FAIL_IF(waitpid(pids[i], &status, 0) != pids[i],
			    "waitpid failed for worker %d", i);
		SCX_FAIL_IF(!WIFEXITED(status) || WEXITSTATUS(status) != 0,
			    "Worker %d did not exit cleanly", i);
	}

	SCX_EQ(skel->data->uei.kind, EXIT_KIND(SCX_EXIT_UNREG));

	return SCX_TEST_PASS;
}

static void cleanup(void *ctx)
{
	bypass__destroy(ctx);
}

struct scx_test bypass_test = {
	.name		= "bypass",
	.description	= "Verify tasks complete during bypass mode",
	.setup		= setup,
	.run		= run,
	.cleanup	= cleanup,
};
REGISTER_SCX_TEST(&bypass_test)
