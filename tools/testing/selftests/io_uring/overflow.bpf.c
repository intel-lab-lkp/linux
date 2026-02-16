/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/types.h>
#include <linux/stddef.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "vmlinux.h"
#include "common.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

const volatile unsigned sq_hdr_offset;
const volatile unsigned cqes_offset;
static unsigned submitted;

SEC("struct_ops.s/overflow_loop_step")
int BPF_PROG(overflow_loop_step, struct io_ring_ctx *ring,
				 struct iou_loop_params *ls)
{
	struct io_uring_sqe *sqes, *sqe;
	struct io_uring_cqe *cqes;
	struct io_uring *sq_hdr;
	void *rings;
	int ret;

	if (submitted >= 2 * SQ_ENTRIES)
		return IOU_LOOP_STOP;

	sqes = (void *)bpf_io_uring_get_region(ring, IOU_REGION_SQ,
				SQ_ENTRIES * sizeof(struct io_uring_sqe));
	rings = (void *)bpf_io_uring_get_region(ring, IOU_REGION_CQ,
				cqes_offset + CQ_ENTRIES * sizeof(struct io_uring_cqe));
	if (!rings || !sqes)
		return IOU_LOOP_STOP;

	sq_hdr = rings + (sq_hdr_offset & 63);
	sqe = &sqes[sq_hdr->tail & (SQ_ENTRIES - 1)];
	*sqe = (struct io_uring_sqe){};
	sqe->opcode = IORING_OP_NOP;
	sq_hdr->tail++;

	ret = bpf_io_uring_submit_sqes(ring, 1);
	if (ret != 1)
		return IOU_LOOP_STOP;

	submitted++;
	return IOU_LOOP_CONTINUE;
}

SEC(".struct_ops.link")
struct io_uring_bpf_ops overflow_ops = {
	.loop_step = (void *)overflow_loop_step,
};