// SPDX-License-Identifier: GPL-2.0
/*
 * A scheduler that reads the NUMA node a task's hinting faults point at.
 *
 * Every task that runs is asked for its preferred node. The answer is either
 * NUMA_NO_NODE, which a task keeps until its address space has been scanned a
 * few times, or a node id the kernel knows about. Anything else means the
 * value reaching BPF is not the one the placement code maintains.
 *
 * Copyright (c) 2026 NVIDIA Corporation.
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

/* Tasks seen with and without a preferred node. */
u64 nr_with_nid;
u64 nr_without_nid;

void BPF_STRUCT_OPS(numa_nid_running, struct task_struct *p)
{
	s32 nid = scx_bpf_task_numa_nid(p);

	if (nid == NUMA_NO_NODE) {
		__sync_fetch_and_add(&nr_without_nid, 1);
		return;
	}

	if (nid < 0 || nid >= scx_bpf_nr_node_ids()) {
		scx_bpf_error("task %d reported node %d, kernel has %d node ids",
			      p->pid, nid, scx_bpf_nr_node_ids());
		return;
	}

	__sync_fetch_and_add(&nr_with_nid, 1);
}

void BPF_STRUCT_OPS(numa_nid_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SEC(".struct_ops.link")
struct sched_ext_ops numa_nid_ops = {
	.running			= (void *)numa_nid_running,
	.exit				= (void *)numa_nid_exit,
	.flags				= SCX_OPS_NUMA_BALANCING,
	.name				= "numa_nid",
};
