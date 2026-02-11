/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/stddef.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>

#include <bpf/libbpf.h>
#include <io_uring/mini_liburing.h>

#include "nops_loop.bpf.skel.h"
#include "common.h"

static struct io_uring_params params;
static struct nops_loop *skel;
static struct bpf_link *nops_loop_link;

#define NR_ITERS 1000

static void setup_ring(struct io_uring *ring)
{
	int ret;

	memset(&params, 0, sizeof(params));
	params.cq_entries = CQ_ENTRIES;
	params.flags = IORING_SETUP_SINGLE_ISSUER |
			IORING_SETUP_DEFER_TASKRUN |
			IORING_SETUP_NO_SQARRAY |
			IORING_SETUP_CQSIZE;

	ret = io_uring_queue_init_params(SQ_ENTRIES, ring, &params);
	if (ret) {
		fprintf(stderr, "ring init failed\n");
		exit(1);
	}
}

static void setup_bpf_ops(struct io_uring *ring)
{
	int ret;

	skel = nops_loop__open();
	if (!skel) {
		fprintf(stderr, "can't generate skeleton\n");
		exit(1);
	}

	skel->struct_ops.nops_loop_ops->ring_fd = ring->ring_fd;
	skel->bss->reqs_to_run = NR_ITERS;
	skel->rodata->sq_hdr_offset = params.sq_off.head;
	skel->rodata->cq_hdr_offset = params.cq_off.head;
	skel->rodata->cqes_offset = params.cq_off.cqes;

	ret = nops_loop__load(skel);
	if (ret) {
		fprintf(stderr, "failed to load skeleton\n");
		exit(1);
	}

	nops_loop_link = bpf_map__attach_struct_ops(skel->maps.nops_loop_ops);
	if (!nops_loop_link) {
		fprintf(stderr, "failed to attach ops\n");
		exit(1);
	}
}

static void run_ring(struct io_uring *ring)
{
	__s64 res[3] = {};
	int i, ret;

	ret = io_uring_enter(ring->ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
	if (ret) {
		fprintf(stderr, "run failed\n");
		exit(1);
	}

	for (i = 0; i < 3; i++) {
		__u32 key = i;

		ret = bpf_map__lookup_elem(skel->maps.res_map,
					&key, sizeof(key),
					&res[i], sizeof(res[i]), 0);
		if (ret)
			fprintf(stderr, "can't read map idx %i: %i\n", i, ret);
	}

	if (res[SLOT_RES] != 1)
		fprintf(stderr, "run failed: %i\n", (int)res[SLOT_RES]);
	if (res[SLOT_NR_CQES] != NR_ITERS)
		fprintf(stderr, "unexpected number of CQEs: %i\n",
			(int)res[SLOT_NR_CQES]);
	if (res[SLOT_NR_SQES] != NR_ITERS)
		fprintf(stderr, "unexpected submitted number: %i\n",
			(int)res[SLOT_NR_SQES]);
}

int main()
{
	struct io_uring ring;

	setup_ring(&ring);
	setup_bpf_ops(&ring);

	run_ring(&ring);

	bpf_link__destroy(nops_loop_link);
	nops_loop__destroy(skel);
	io_uring_queue_exit(&ring);
	return 0;
}
