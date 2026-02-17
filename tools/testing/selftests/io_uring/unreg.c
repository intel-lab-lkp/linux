/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/stddef.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>

#include <bpf/libbpf.h>
#include <io_uring/mini_liburing.h>

#include "unreg.bpf.skel.h"
#include "common.h"

static void setup_ring(struct io_uring *ring)
{
	struct io_uring_params params;
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

static struct unreg *load_unreg(struct io_uring *ring)
{
	struct unreg *skel;
	int ret;

	skel = unreg__open();
	if (!skel) {
		fprintf(stderr, "can't generate skeleton\n");
		exit(1);
	}

	skel->struct_ops.unreg_ops->ring_fd = ring->ring_fd;

	ret = unreg__load(skel);
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
	struct bpf_link *link1, *link2;
	struct unreg *skel1, *skel2;
	struct io_uring_sqe *sqe;
	struct io_uring ring;

	setup_ring(&ring);
	sqe = &ring.sq.sqes[0];
	sqe->user_data = 0;

	skel1 = load_unreg(&ring);
	skel2 = load_unreg(&ring);

	link1 = bpf_map__attach_struct_ops(skel1->maps.unreg_ops);
	if (!link1) {
		fprintf(stderr, "failed to attach ops\n");
		return 1;
	}

	run_ring(&ring);
	if (sqe->user_data != 1) {
		fprintf(stderr, "failed to run BPF\n");
		return 1;
	}

	/* remove the program and give the kernel time to actually destroy it */
	bpf_link__destroy(link1);
	unreg__destroy(skel1);
	sleep(1);

	run_ring(&ring);
	if (sqe->user_data != 1) {
		fprintf(stderr, "Executed removed BPF\n");
		return 1;
	}

	/* try to attach another program */
	link2 = bpf_map__attach_struct_ops(skel2->maps.unreg_ops);
	if (!link2) {
		fprintf(stderr, "failed to reattach ops\n");
		return 1;
	}

	run_ring(&ring);
	if (sqe->user_data != 2) {
		fprintf(stderr, "failed to run reattached BPF\n");
		return 1;
	}

	bpf_link__destroy(link2);
	unreg__destroy(skel2);
	io_uring_queue_exit(&ring);
	return 0;
}
