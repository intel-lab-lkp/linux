// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Andrea Righi <arighi@nvidia.com>
 */
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

static int test_select_cpu_from_user(const struct allowed_cpus *skel)
{
	int fd, ret;
	__u64 args[1];

	LIBBPF_OPTS(bpf_test_run_opts, attr,
		.ctx_in = args,
		.ctx_size_in = sizeof(args),
	);

	args[0] = getpid();
	fd = bpf_program__fd(skel->progs.select_cpu_from_user);
	if (fd < 0)
		return fd;

	ret = bpf_prog_test_run_opts(fd, &attr);
	if (ret < 0)
		return ret;

	fprintf(stderr, "%s: CPU %d\n", __func__, attr.retval);

	return 0;
}

/*
 * Run this task once on every CPU while ops.running() repairs the bootstrap
 * idle state. Once a CPU has been refreshed, subsequent idle transitions keep
 * its state up to date.
 */
static int refresh_idle_masks(void)
{
	cpu_set_t original, one;
	int cpu, ret = 0;

	if (sched_getaffinity(0, sizeof(original), &original))
		return -errno;

	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &original))
			continue;

		CPU_ZERO(&one);
		CPU_SET(cpu, &one);
		if (sched_setaffinity(0, sizeof(one), &one)) {
			ret = -errno;
			break;
		}

		sched_yield();
	}

	if (sched_setaffinity(0, sizeof(original), &original) && !ret)
		ret = -errno;

	return ret;
}

static enum scx_test_status run(void *ctx)
{
	struct allowed_cpus *skel = ctx;
	struct bpf_link *link;

	skel->bss->refresh_idle_masks = true;
	link = bpf_map__attach_struct_ops(skel->maps.allowed_cpus_ops);
	SCX_FAIL_IF(!link, "Failed to attach scheduler");

	SCX_FAIL_IF(refresh_idle_masks(), "Failed to refresh idle CPU state");
	__atomic_store_n(&skel->bss->refresh_idle_masks, false, __ATOMIC_RELEASE);

	/* Pick an idle CPU from user-space */
	SCX_FAIL_IF(test_select_cpu_from_user(skel), "Failed to pick idle CPU");

	/* Just sleeping is fine, plenty of scheduling events happening */
	sleep(1);

	SCX_EQ(skel->data->uei.kind, EXIT_KIND(SCX_EXIT_NONE));
	bpf_link__destroy(link);

	return SCX_TEST_PASS;
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
