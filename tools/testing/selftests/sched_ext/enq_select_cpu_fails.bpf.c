/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2023 David Vernet <dvernet@meta.com>
 * Copyright (c) 2023 Tejun Heo <tj@kernel.org>
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

s32 BPF_STRUCT_OPS(enq_select_cpu_fails_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	return prev_cpu;
}

void BPF_STRUCT_OPS(enq_select_cpu_fails_enqueue, struct task_struct *p,
		    u64 enq_flags)
{
	/* Can only call from ops.select_cpu() */
	scx_bpf_select_cpu_and(p, p->cpus_ptr, 0, 0, 0);

	scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
}

SEC(".struct_ops.link")
struct sched_ext_ops enq_select_cpu_fails_ops = {
	.select_cpu		= (void *) enq_select_cpu_fails_select_cpu,
	.enqueue		= (void *) enq_select_cpu_fails_enqueue,
	.name			= "enq_select_cpu_fails",
	.timeout_ms		= 1000U,
};
