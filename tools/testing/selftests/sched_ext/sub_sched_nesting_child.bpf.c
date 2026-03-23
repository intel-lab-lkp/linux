/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Nesting-capable child scheduler for multi-level sub-sched testing.
 *
 * This scheduler can itself act as a parent for deeper nesting levels,
 * allowing us to test cascading disable and cleanup of nested schedulers.
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

__u64 dispatch_count;

void BPF_STRUCT_OPS(nesting_child_dispatch, s32 cpu, struct task_struct *prev)
{
	__sync_fetch_and_add(&dispatch_count, 1);
}

/* This child can itself be a parent to further nesting */
s32 BPF_STRUCT_OPS(nesting_child_sub_attach, struct scx_sub_attach_args *args)
{
	/* Accept sub_attach to allow deeper nesting */
	return 0;
}

void BPF_STRUCT_OPS(nesting_child_sub_detach, struct scx_sub_detach_args *args)
{
	/* Detach handling */
}

SEC(".struct_ops.link")
struct sched_ext_ops sub_sched_nesting_child_ops = {
	.name			= "sub_sched_nesting_child",
	.sub_cgroup_id		= 0,  /* Will be set by user space */
	.dispatch		= (void *)nesting_child_dispatch,
	.sub_attach		= (void *)nesting_child_sub_attach,
	.sub_detach		= (void *)nesting_child_sub_detach,
};
