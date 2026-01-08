// SPDX-License-Identifier: GPL-2.0
/*
 * A BPF program for testing DSQ statistics functionality.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 */

#include <scx/common.bpf.h>
#include <scx/compat.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei); /* Error handling */

#define TEST_DSQ_ID 1234

/* Test variables to store results */
s64 test_enqueue_count = -1;
s64 test_dequeue_count = -1;
s32 test_peak_nr = -1;
s32 current_nr = -1;

/* Test completion flag */
int test_completed;

/* Test task */
int test_pid = -1;

static void test_dsq_stats(void)
{
	s64 enqueue_count, dequeue_count;
	s32 peak_nr, nr;

	/* Test the new statistics functions */
	enqueue_count = __COMPAT_scx_bpf_dsq_enqueue_count(TEST_DSQ_ID);
	dequeue_count = __COMPAT_scx_bpf_dsq_dequeue_count(TEST_DSQ_ID);
	peak_nr = __COMPAT_scx_bpf_dsq_peak_nr(TEST_DSQ_ID);
	nr = scx_bpf_dsq_nr_queued(TEST_DSQ_ID);

	/* Store results */
	test_enqueue_count = enqueue_count;
	test_dequeue_count = dequeue_count;
	test_peak_nr = peak_nr;
	current_nr = nr;

	test_completed = 1;
}

void BPF_STRUCT_OPS(dsq_stats_enqueue, struct task_struct *p, u64 enq_flags)
{
	/* Create DSQ on first task */
	if (test_pid == -1) {
		test_pid = p->pid;

		/* Create test DSQ */
		if (scx_bpf_create_dsq(TEST_DSQ_ID, -1)) {
			bpf_printk("Failed to create test DSQ\n");
			return;
		}

		/* Insert task into DSQ to test statistics */
		scx_bpf_dsq_insert(p, TEST_DSQ_ID, 0, enq_flags);
	}
}

void BPF_STRUCT_OPS(dsq_stats_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Run test if not completed */
	if (!test_completed && test_pid != -1) {
		test_dsq_stats();

		/* Consume the task to complete the test */
		scx_bpf_dsq_move_to_local(TEST_DSQ_ID);
	}
}

s32 BPF_STRUCT_OPS_SLEEPABLE(dsq_stats_init)
{
	/* Initialize test variables */
	test_pid = -1;
	test_completed = 0;

	return 0;
}

void BPF_STRUCT_OPS(dsq_stats_exit, struct scx_exit_info *ei)
{
	/* Destroy test DSQ */
	scx_bpf_destroy_dsq(TEST_DSQ_ID);

	UEI_RECORD(uei, ei);
}

SEC(".struct_ops.link")
struct sched_ext_ops dsq_stats_ops = {
	.enqueue = (void *)dsq_stats_enqueue,
	.dispatch = (void *)dsq_stats_dispatch,
	.init = (void *)dsq_stats_init,
	.exit = (void *)dsq_stats_exit,
	.name = "dsq_stats",
};
