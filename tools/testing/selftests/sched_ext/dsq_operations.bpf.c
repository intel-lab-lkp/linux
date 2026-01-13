/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF scheduler for DSQ operations testing
 *
 * Copyright (c) 2025 Linux Kernel Contributors
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

#define TEST_DSQ_ID 1
#define MAX_DSQ_SIZE 100

/* Track DSQ state */
static u64 dsq_insert_count = 0;
static u64 dsq_remove_count = 0;

/* Test helper: Check DSQ bounds */
static bool is_dsq_full(u64 dsq_id)
{
	/* In real implementation, we'd query DSQ stats */
	/* For testing, we simulate with a counter */
	return dsq_insert_count >= MAX_DSQ_SIZE;
}

/* Test helper: Validate task state */
static bool validate_task_state(struct task_struct *p)
{
	if (!p)
		return false;
	
	/* Check if task is in valid state for scheduling */
	if (p->state & (TASK_DEAD | TASK_WAKING))
		return false;
	
	return true;
}

/* Test: Basic enqueue with validation */
void BPF_STRUCT_OPS(dsq_ops_enqueue, struct task_struct *p, u64 enq_flags)
{
	if (!validate_task_state(p)) {
		/* Should not happen in normal operation */
		scx_bpf_error("Invalid task state in enqueue");
		return;
	}

	/* Test DSQ insertion with different priorities */
	if (is_dsq_full(TEST_DSQ_ID)) {
		/* Fallback to global DSQ when full */
		scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
		return;
	}

	/* Insert into test DSQ */
	scx_bpf_dsq_insert(p, TEST_DSQ_ID, SCX_SLICE_DFL, enq_flags);
	__sync_fetch_and_add(&dsq_insert_count, 1);
}

/* Test: Dispatch with error handling */
void BPF_STRUCT_OPS(dsq_ops_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Test: Move from test DSQ to local */
	s32 ret = scx_bpf_dsq_move_to_local(TEST_DSQ_ID);
	
	if (ret < 0) {
		/* Handle error case */
		scx_bpf_error("Failed to move from DSQ: %d", ret);
		return;
	}

	/* Test: Also try moving from global if local is empty */
	if (ret == 0) {
		scx_bpf_dsq_move_to_local(SCX_DSQ_GLOBAL);
	}
}

/* Test: Running callback with state validation */
void BPF_STRUCT_OPS(dsq_ops_running, struct task_struct *p)
{
	if (!validate_task_state(p)) {
		scx_bpf_error("Invalid task in running state");
		return;
	}
}

/* Test: Stopping callback with runnable flag */
void BPF_STRUCT_OPS(dsq_ops_stopping, struct task_struct *p, bool runnable)
{
	if (runnable) {
		/* Task will be re-enqueued, verify it's still valid */
		if (!validate_task_state(p)) {
			scx_bpf_error("Invalid runnable task in stopping");
		}
	}
}

/* Test: Update idle with CPU validation */
void BPF_STRUCT_OPS(dsq_ops_update_idle, s32 cpu, bool idle)
{
	if (cpu < 0 || cpu >= nr_cpu_ids) {
		scx_bpf_error("Invalid CPU ID: %d", cpu);
		return;
	}

	/* Test: Track idle transitions */
	if (idle) {
		/* CPU entering idle */
	} else {
		/* CPU exiting idle */
	}
}

/* Test: Set weight with bounds checking */
void BPF_STRUCT_OPS(dsq_ops_set_weight, struct task_struct *p, u32 weight)
{
	if (weight < 1 || weight > 10000) {
		scx_bpf_error("Invalid weight value: %u", weight);
		return;
	}
}

/* Test: Set cpumask with validation */
void BPF_STRUCT_OPS(dsq_ops_set_cpumask, struct task_struct *p,
		   const struct cpumask *cpumask)
{
	if (!cpumask) {
		scx_bpf_error("NULL cpumask");
		return;
	}

	/* Verify cpumask is not empty */
	if (bpf_cpumask_empty(cpumask)) {
		scx_bpf_error("Empty cpumask");
		return;
	}
}

/* Test: Yield operation */
bool BPF_STRUCT_OPS(dsq_ops_yield, struct task_struct *from,
		   struct task_struct *to)
{
	/* Test: Validate both tasks */
	if (!validate_task_state(from)) {
		return false;
	}

	if (to && !validate_task_state(to)) {
		return false;
	}

	/* Allow yield to specific task or any */
	return true;
}

/* Test: Initialization */
s32 BPF_STRUCT_OPS_SLEEPABLE(dsq_ops_init)
{
	s32 ret;

	/* Test: Create DSQ with different flags */
	ret = scx_bpf_create_dsq(TEST_DSQ_ID, -1);
	if (ret < 0) {
		scx_bpf_error("Failed to create test DSQ: %d", ret);
		return ret;
	}

	return 0;
}

/* Test: Cleanup */
void BPF_STRUCT_OPS(dsq_ops_exit, struct scx_exit_info *info)
{
	/* Test: Verify exit info */
	if (!info) {
		scx_bpf_error("NULL exit info");
		return;
	}

	/* Log exit reason for debugging */
	if (info->reason == SCX_EXIT_ERROR) {
		scx_bpf_error("Scheduler exiting due to error: %s", info->msg);
	}
}

SEC(".struct_ops.link")
struct sched_ext_ops dsq_ops = {
	.enqueue		= (void *) dsq_ops_enqueue,
	.dispatch		= (void *) dsq_ops_dispatch,
	.running		= (void *) dsq_ops_running,
	.stopping		= (void *) dsq_ops_stopping,
	.update_idle		= (void *) dsq_ops_update_idle,
	.set_weight		= (void *) dsq_ops_set_weight,
	.set_cpumask		= (void *) dsq_ops_set_cpumask,
	.yield			= (void *) dsq_ops_yield,
	.init			= (void *) dsq_ops_init,
	.exit			= (void *) dsq_ops_exit,
	.name			= "dsq_operations",
};