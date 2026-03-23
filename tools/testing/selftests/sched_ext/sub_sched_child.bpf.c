/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Child scheduler for sub-sched testing.
 *
 * This is a minimal scheduler that attaches to a specific cgroup via
 * sub_cgroup_id. The BPF loader will populate sub_cgroup_id at load time
 * before attaching.
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

__u64 dispatch_count;

void BPF_STRUCT_OPS(sub_sched_child_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Minimal dispatch: just return without dispatching anything.
	 * The kernel will use the default scheduling paths.
	 */
	__sync_fetch_and_add(&dispatch_count, 1);
}

SEC(".struct_ops.link")
struct sched_ext_ops sub_sched_child_ops = {
	.name			= "sub_sched_child",
	.sub_cgroup_id		= 0,  /* Will be set by user space */
	.dispatch		= (void *)sub_sched_child_dispatch,
};
