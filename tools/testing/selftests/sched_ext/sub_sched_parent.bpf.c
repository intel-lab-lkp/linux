/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Parent scheduler for sub-sched testing.
 *
 * This scheduler demonstrates the sub_attach/sub_detach ops that allow
 * a parent scheduler to manage sub-schedulers attached to specific cgroups.
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

/* Simple counter for diagnostics */
__u64 attach_count;
__u64 detach_count;

s32 BPF_STRUCT_OPS(parent_sub_attach, struct scx_sub_attach_args *args)
{
	__sync_fetch_and_add(&attach_count, 1);
	return 0;
}

void BPF_STRUCT_OPS(parent_sub_detach, struct scx_sub_detach_args *args)
{
	__sync_fetch_and_add(&detach_count, 1);
}

SEC(".struct_ops.link")
struct sched_ext_ops sub_sched_parent_ops = {
	.name			= "sub_sched_parent",
	.sub_attach		= (void *)parent_sub_attach,
	.sub_detach		= (void *)parent_sub_detach,
};
