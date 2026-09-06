// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Andrea Righi <arighi@nvidia.com>
 */
#define _GNU_SOURCE
#include <bpf/bpf.h>
#include <sched.h>
#include <scx/common.h>
#include <sys/wait.h>
#include <unistd.h>
#include "allowed_cpus.bpf.skel.h"
#include "scx_test.h"

static enum scx_test_status setup(void **ctx)
{
	struct allowed_cpus *skel;

	skel = allowed_cpus__open();
	SCX_FAIL_IF(!skel, "Failed to open");
	SCX_ENUM_INIT(skel);
	SCX_FAIL_IF(allowed_cpus__load(skel), "Failed to load skel");

	*ctx = skel;

	return SCX_TEST_PASS;
}

static int test_select_cpu_from_user(const struct allowed_cpus *skel,
				     const char *name, int custom_cpu,
				     bool expect_busy)
{
	int fd, ret;
	__s32 cpu;
	__u64 args[] = { getpid(), (__u64)(__s64)custom_cpu };

	LIBBPF_OPTS(bpf_test_run_opts, attr,
		.ctx_in = args,
		.ctx_size_in = sizeof(args),
	);

	fd = bpf_program__fd(skel->progs.select_cpu_from_user);
	if (fd < 0)
		return fd;

	ret = bpf_prog_test_run_opts(fd, &attr);
	if (ret < 0)
		return ret;

	/* test_run returns the signed BPF result through an unsigned field. */
	cpu = (__s32)attr.retval;
	if ((expect_busy && cpu != -EBUSY) ||
	    (!expect_busy && cpu != -EBUSY && cpu != custom_cpu)) {
		SCX_ERR("%s: unexpected CPU selection result %d", name, cpu);
		return -EINVAL;
	}

	return 0;
}

static enum scx_test_status run(void *ctx)
{
	struct allowed_cpus *skel = ctx;
	enum scx_test_status status = SCX_TEST_FAIL;
	cpu_set_t original, pinned;
	int first = -1, second = -1, cpu;
	struct bpf_link *link;

	SCX_FAIL_IF(sched_getaffinity(0, sizeof(original), &original),
		    "Failed to get affinity (%d)", errno);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &original))
			continue;
		if (first < 0)
			first = cpu;
		else {
			second = cpu;
			break;
		}
	}
	SCX_FAIL_IF(first < 0, "No CPU in affinity mask");

	link = bpf_map__attach_struct_ops(skel->maps.allowed_cpus_ops);
	SCX_FAIL_IF(!link, "Failed to attach scheduler");

	if (test_select_cpu_from_user(skel, "empty mask", -1, true))
		goto out;

	/* A legal candidate may be busy; selection need not succeed. */
	if (test_select_cpu_from_user(skel, "legal candidate", first, false))
		goto out;

	if (second >= 0) {
		CPU_ZERO(&pinned);
		CPU_SET(first, &pinned);
		if (sched_setaffinity(0, sizeof(pinned), &pinned)) {
			SCX_ERR("Failed to pin task (%d)", errno);
			goto out;
		}
		if (test_select_cpu_from_user(skel, "disjoint masks", second, true))
			goto restore;
	} else {
		fprintf(stderr, "Skipping disjoint masks: need two allowed CPUs\n");
	}

	/* Just sleeping is fine, plenty of scheduling events happening. */
	sleep(1);
	if (skel->data->uei.kind != EXIT_KIND(SCX_EXIT_NONE)) {
		SCX_ERR("Scheduler exited unexpectedly");
		goto restore;
	}
	status = SCX_TEST_PASS;

restore:
	if (second >= 0 && sched_setaffinity(0, sizeof(original), &original)) {
		SCX_ERR("Failed to restore affinity (%d)", errno);
		status = SCX_TEST_FAIL;
	}
out:
	bpf_link__destroy(link);
	return status;
}

static void cleanup(void *ctx)
{
	struct allowed_cpus *skel = ctx;

	allowed_cpus__destroy(skel);
}

struct scx_test allowed_cpus = {
	.name = "allowed_cpus",
	.description = "Verify scx_bpf_select_cpu_and()",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&allowed_cpus)
