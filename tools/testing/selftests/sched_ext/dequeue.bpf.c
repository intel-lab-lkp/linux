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
 * - dequeue_cnt: Number of times ops.dequeue() was called (any type)
 * - dispatch_dequeue_cnt: Number of regular dispatch dequeues (no flag)
 * - async_dequeue_cnt: Number of async dequeues (SCX_DEQ_ASYNC)
 */
u64 enqueue_cnt, dequeue_cnt, dispatch_dequeue_cnt, async_dequeue_cnt;

/*
 * Test scenarios:
 * - 0: Dispatch to local DSQ
 * - 1: Dispatch to shared DSQ
 */
u32 test_scenario;

/*
 * Per-task state to track lifecycle and validate workflow semantics.
 * State transitions:
 *   NONE -> ENQUEUED (on enqueue)
 *   ENQUEUED -> DISPATCHED (on dispatch dequeue)
 *   DISPATCHED -> NONE (on async dequeue or re-enqueue)
 *   ENQUEUED -> NONE (on async dequeue before dispatch)
 */
enum task_state {
	TASK_NONE = 0,      /* Task is outside scheduler control */
	TASK_ENQUEUED,      /* ops.enqueue() called, waiting for dequeue */
	TASK_DISPATCHED,    /* Dispatch dequeue received, can get async or re-enqueue */
};

struct task_ctx {
	enum task_state state; /* Current state in the workflow */
	u64 enqueue_seq;       /* Sequence number for debugging */
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

	/*
	 * Validate state transition: enqueue is only valid from NONE or
	 * DISPATCHED states. Getting enqueue while in ENQUEUED state
	 * indicates a missing dequeue.
	 */
	if (tctx->state == TASK_ENQUEUED)
		scx_bpf_error("%d (%s): enqueue while in ENQUEUED state (seq %llu)",
			      p->pid, p->comm, tctx->enqueue_seq);

	/* Transition to ENQUEUED state */
	tctx->state = TASK_ENQUEUED;
	tctx->enqueue_seq++;

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

	/*
	 * Validate state: dequeue should only happen from ENQUEUED or
	 * DISPATCHED states. Getting dequeue from NONE indicates a bug.
	 */
	if (tctx->state == TASK_NONE)
		scx_bpf_error("%d (%s): dequeue from NONE state (seq %llu)",
			      p->pid, p->comm, tctx->enqueue_seq);

	if (deq_flags & SCX_DEQ_ASYNC) {
		/*
		 * Async dequeue: property change interrupting the workflow.
		 * Valid from both ENQUEUED and DISPATCHED states.
		 * Transitions task back to NONE state.
		 */
		__sync_fetch_and_add(&async_dequeue_cnt, 1);

		/* Validate state transition */
		if (tctx->state != TASK_ENQUEUED && tctx->state != TASK_DISPATCHED)
			scx_bpf_error("%d (%s): async dequeue from invalid state %d (seq %llu)",
				      p->pid, p->comm, tctx->state, tctx->enqueue_seq);

		/* Transition back to NONE - task outside scheduler control */
		tctx->state = TASK_NONE;
	} else {
		/*
		 * Regular dispatch dequeue: normal workflow step.
		 * Valid only from ENQUEUED state (after enqueue, before dispatch dequeue).
		 * Transitions to DISPATCHED state.
		 */
		__sync_fetch_and_add(&dispatch_dequeue_cnt, 1);

		/* Validate: dispatch dequeue should NOT have SCX_DEQ_ASYNC flag */
		if (deq_flags & SCX_DEQ_ASYNC)
			scx_bpf_error("%d (%s): SCX_DEQ_ASYNC in dispatch dequeue (seq %llu)",
				      p->pid, p->comm, tctx->enqueue_seq);

		/* Must be in ENQUEUED state */
		if (tctx->state != TASK_ENQUEUED)
			scx_bpf_error("%d (%s): dispatch dequeue from state %d (seq %llu)",
				      p->pid, p->comm, tctx->state, tctx->enqueue_seq);

		/* Transition to DISPATCHED - normal cycle completed dispatch */
		tctx->state = TASK_DISPATCHED;
	}
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
