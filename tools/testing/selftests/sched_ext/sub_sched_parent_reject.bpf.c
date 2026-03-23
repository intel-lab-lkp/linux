/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Parent scheduler that rejects sub_attach for failure path testing.
 *
 * This scheduler deliberately rejects sub_attach to test that the kernel
 * properly cleans up partially-initialized tasks and rolls back without crashing.
 *
 * Copyright (c) 2026 Xiaomi Corporation.
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

__u64 attach_reject_count;

s32 BPF_STRUCT_OPS(parent_reject_sub_attach, struct scx_sub_attach_args *args)
{
	/* Deliberately reject sub_attach to trigger rollback path */
	__sync_fetch_and_add(&attach_reject_count, 1);
	return -EPERM;
}

SEC(".struct_ops.link")
struct sched_ext_ops sub_sched_parent_reject_ops = {
	.name			= "sub_sched_parent_reject",
	.sub_attach		= (void *)parent_reject_sub_attach,
};
