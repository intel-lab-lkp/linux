// SPDX-License-Identifier: GPL-2.0
/*
 * BPF scheduler for bypass mode operational test.
 *
 * Implements a minimal global FIFO scheduler. The userspace side
 * attaches this scheduler, runs worker tasks to completion, and
 * verifies that tasks complete successfully.
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

void BPF_STRUCT_OPS(bypass_enqueue, struct task_struct *p, u64 enq_flags)
{
	scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
}

void BPF_STRUCT_OPS(bypass_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SEC(".struct_ops.link")
struct sched_ext_ops bypass_ops = {
	.enqueue		= (void *)bypass_enqueue,
	.exit			= (void *)bypass_exit,
	.name			= "bypass_test",
};
