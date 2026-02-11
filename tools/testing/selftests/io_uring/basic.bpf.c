/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/types.h>
#include <linux/stddef.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "vmlinux.h"
#include "common.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#define REQ_TOKEN 0xabba1741

const unsigned max_inflight = 8;
const volatile unsigned cq_hdr_offset;
const volatile unsigned sq_hdr_offset;
const volatile unsigned cqes_offset;

int reqs_to_run;
unsigned inflight;

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 3);
	__type(key, u32);
	__type(value, s64);
} res_map SEC(".maps");

#define t_min(a, b) ((a) < (b) ? (a) : (b))

static inline void set_cq_wait(struct iou_loop_params *lp,
			       struct io_uring *cq_hdr, unsigned to_wait)
{
	lp->cq_wait_idx = cq_hdr->head + to_wait;
}

static inline void write_result(int res)
{
	u32 key = SLOT_RES;
	u64 *val;

	val = bpf_map_lookup_elem(&res_map, &key);
	if (val)
		*val = res;
}

static inline void write_stats(int idx, unsigned int v)
{
	u32 key = idx;
	u64 *val;

	val = bpf_map_lookup_elem(&res_map, &key);
	if (val)
		*val += v;
}

SEC("struct_ops.s/link_loop")
int BPF_PROG(link_loop, struct io_ring_ctx *ring, struct iou_loop_params *ls)
{
	struct io_uring *sq_hdr, *cq_hdr;
	struct io_uring_sqe *sqes;
	struct io_uring_cqe *cqes;
	void *rings;
	int ret;

	sqes = (void *)bpf_io_uring_get_region(ring, IOU_REGION_SQ,
				SQ_ENTRIES * sizeof(struct io_uring_sqe));
	rings = (void *)bpf_io_uring_get_region(ring, IOU_REGION_CQ,
				cqes_offset + CQ_ENTRIES * sizeof(struct io_uring_cqe));
	if (!rings || !sqes) {
		write_result(-1);
		return IOU_LOOP_STOP;
	}

	sq_hdr = rings + (sq_hdr_offset & 63);
	cq_hdr = rings + (cq_hdr_offset & 63);
	cqes = rings + cqes_offset;

	unsigned to_wait = cq_hdr->tail - cq_hdr->head;
	to_wait = t_min(to_wait, CQ_ENTRIES);
	for (int i = 0; i < to_wait; i++) {
		struct io_uring_cqe *cqe = &cqes[cq_hdr->head & (CQ_ENTRIES - 1)];

		if (cqe->user_data != REQ_TOKEN) {
			write_result(-3);
			return IOU_LOOP_STOP;
		}
		cq_hdr->head++;
	}

	reqs_to_run -= to_wait;
	inflight -= to_wait;
	write_stats(SLOT_NR_CQES, to_wait);

	if (reqs_to_run <= 0) {
		write_result(1);
		return IOU_LOOP_STOP;
	}

	if (inflight < max_inflight) {
		unsigned to_submit = max_inflight - inflight;

		to_submit = t_min(to_submit, reqs_to_run);

		for (int i = 0; i < to_submit; i++) {
			struct io_uring_sqe *sqe = sqes + i;

			sqe = &sqes[sq_hdr->tail & (SQ_ENTRIES - 1)];
			*sqe = (struct io_uring_sqe){};
			sqe->opcode = IORING_OP_NOP;
			sqe->user_data = REQ_TOKEN;
			sq_hdr->tail++;
		}

		ret = bpf_io_uring_submit_sqes(ring, to_submit);
		if (ret != to_submit) {
			write_result(-2);
			return IOU_LOOP_STOP;
		}

		inflight += to_submit;
		write_stats(SLOT_NR_SQES, to_submit);
	}

	set_cq_wait(ls, cq_hdr, 1);
	return IOU_LOOP_CONTINUE;
}

SEC(".struct_ops.link")
struct io_uring_bpf_ops basic_ops = {
	.loop_step = (void *)link_loop,
};
