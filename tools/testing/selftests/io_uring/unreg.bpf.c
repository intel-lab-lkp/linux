/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/types.h>
#include <linux/stddef.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "vmlinux.h"
#include "common.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

SEC("struct_ops.s/dummy")
int BPF_PROG(dummy, struct io_ring_ctx *ring, struct iou_loop_params *ls)
{
	struct io_uring_sqe *sqes;

	sqes = (void *)bpf_io_uring_get_region(ring, IOU_REGION_SQ,
						sizeof(struct io_uring_sqe));
	if (sqes)
		sqes->user_data++;
	return IOU_LOOP_STOP;
}

SEC(".struct_ops.link")
struct io_uring_bpf_ops unreg_ops = {
	.loop_step = (void *)dummy,
};
