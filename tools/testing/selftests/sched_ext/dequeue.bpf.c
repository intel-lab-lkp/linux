// SPDX-License-Identifier: GPL-2.0
/*
 * A scheduler that validates ops.dequeue() is called correctly:
 * - For tasks on BPF data structures (not yet dispatched)
 * - For tasks already on DSQs (local or shared)
 * - That every ops.enqueue() is followed by ops.dequeue()
 *
 * Copyright (c) 2025 NVIDIA Corporation.
 */

#include <scx/common.bpf.h>

#define SHARED_DSQ	0

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

/*
 * Counters to track the lifecycle of tasks:
 * - enqueue_cnt: Number of times ops.enqueue() was called
 * - dequeue_cnt: Number of times ops.dequeue() was called
 */
u64 enqueue_cnt, dequeue_cnt;

/*
 * Test scenarios:
 * - 0: Dispatch to local DSQ
 * - 1: Dispatch to shared DSQ
 */
u32 test_scenario;

/* Per-task state */
struct task_ctx {
	u64 enqueued;	/* was this task enqueued? */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

static struct task_ctx *try_lookup_task_ctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

s32 BPF_STRUCT_OPS(dequeue_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	/* Always bounce to ops.enqueue() */
	return prev_cpu;
}

void BPF_STRUCT_OPS(dequeue_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx;

	__sync_fetch_and_add(&enqueue_cnt, 1);

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;

	tctx->enqueued = 1;

	switch (test_scenario) {
	case 0:
		/* Scenario 0: Direct dispatch to the local DSQ */
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, enq_flags);
		break;

	case 1:
		/* Scenario 1: Dispatch to shared DSQ */
		scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
		break;
	}
}

void BPF_STRUCT_OPS(dequeue_dequeue, struct task_struct *p, u64 deq_flags)
{
	struct task_ctx *tctx;

	__sync_fetch_and_add(&dequeue_cnt, 1);

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;

	tctx->enqueued = 0;
}

void BPF_STRUCT_OPS(dequeue_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

s32 BPF_STRUCT_OPS(dequeue_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct task_ctx *tctx;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				   BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;

	return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(dequeue_init)
{
	s32 ret;

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret)
		return ret;

	return 0;
}

void BPF_STRUCT_OPS(dequeue_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SEC(".struct_ops.link")
struct sched_ext_ops dequeue_ops = {
	.select_cpu		= (void *)dequeue_select_cpu,
	.enqueue		= (void *)dequeue_enqueue,
	.dequeue		= (void *)dequeue_dequeue,
	.dispatch		= (void *)dequeue_dispatch,
	.init_task		= (void *)dequeue_init_task,
	.init			= (void *)dequeue_init,
	.exit			= (void *)dequeue_exit,
	.name			= "dequeue_test",
};
