/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/stddef.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>

#include <bpf/libbpf.h>
#include <io_uring/mini_liburing.h>

#include "overflow.bpf.skel.h"
#include "common.h"

static struct io_uring_params params;

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

static struct overflow *load_overflow(struct io_uring *ring)
{
	struct overflow *skel;
	int ret;

	skel = overflow__open();
	if (!skel) {
		fprintf(stderr, "can't generate skeleton\n");
		exit(1);
	}

	skel->struct_ops.overflow_ops->ring_fd = ring->ring_fd;
	skel->rodata->sq_hdr_offset = params.sq_off.head;
	skel->rodata->cqes_offset = params.cq_off.cqes;

	ret = overflow__load(skel);
	if (ret) {
		fprintf(stderr, "failed to load skeleton\n");
		exit(1);
	}

	return skel;
}

static void run_ring(struct io_uring *ring)
{
	io_uring_enter(ring->ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
}

int main()
{
	struct bpf_link *link;
	struct io_uring ring;
	struct overflow *skel;

	setup_ring(&ring);
	skel = load_overflow(&ring);
	link = bpf_map__attach_struct_ops(skel->maps.overflow_ops);
	if (!link) {
		fprintf(stderr, "failed to attach ops\n");
		return 1;
	}

	run_ring(&ring);

	bpf_link__destroy(link);
	overflow__destroy(skel);
	io_uring_queue_exit(&ring);
	return 0;
}
