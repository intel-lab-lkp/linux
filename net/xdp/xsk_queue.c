// SPDX-License-Identifier: GPL-2.0
/* XDP user-space ring structure
 * Copyright(c) 2018 Intel Corporation.
 */

#include <linux/log2.h>
#include <linux/slab.h>
#include <linux/overflow.h>
#include <linux/vmalloc.h>
#include <net/sock.h>
#include <net/xdp_sock_drv.h>

#include "xsk_queue.h"

static size_t xskq_get_ring_size(struct xsk_queue *q, bool umem_queue)
{
	struct xdp_umem_ring *umem_ring;
	struct xdp_rxtx_ring *rxtx_ring;

	if (umem_queue)
		return struct_size(umem_ring, desc, q->nentries);
	return struct_size(rxtx_ring, desc, q->nentries);
}

static bool xskq_charge(struct sock *sk, size_t size)
{
	int optmem_max = READ_ONCE(sock_net(sk)->core.sysctl_optmem_max);
	int old, new;

	if (optmem_max <= 0 || size > optmem_max)
		return false;

	do {
		old = atomic_read(&sk->sk_omem_alloc);
		if (old < 0 || old > optmem_max - (int)size)
			return false;
		new = old + (int)size;
	} while (atomic_cmpxchg(&sk->sk_omem_alloc, old, new) != old);

	return true;
}

struct xsk_queue *xskq_create(struct sock *sk, u32 nentries, bool umem_queue)
{
	struct xsk_queue *q;
	size_t size;

	q = kzalloc_obj(*q);
	if (!q)
		return NULL;

	q->nentries = nentries;
	q->ring_mask = nentries - 1;

	size = xskq_get_ring_size(q, umem_queue);

	/* size which is overflowing or close to SIZE_MAX will become 0 in
	 * PAGE_ALIGN(), checking SIZE_MAX is enough due to the previous
	 * is_power_of_2(), the rest will be handled by vmalloc_user()
	 */
	if (unlikely(size == SIZE_MAX)) {
		kfree(q);
		return NULL;
	}

	size = PAGE_ALIGN(size);
	if (!xskq_charge(sk, size)) {
		kfree(q);
		return NULL;
	}

	sock_hold(sk);
	q->sk = sk;

	q->ring = vmalloc_user(size);
	if (!q->ring) {
		atomic_sub((int)size, &sk->sk_omem_alloc);
		sock_put(sk);
		kfree(q);
		return NULL;
	}

	q->ring_vmalloc_size = size;
	return q;
}

void xskq_destroy(struct xsk_queue *q)
{
	if (!q)
		return;

	if (q->sk) {
		atomic_sub((int)q->ring_vmalloc_size, &q->sk->sk_omem_alloc);
		sock_put(q->sk);
	}

	vfree(q->ring);
	kfree(q);
}
