/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __XSK_H__
#define __XSK_H__

#define VIRTIO_XSK_FLAG_OFFSET	4

static inline void *virtnet_xsk_to_ptr(u32 len)
{
	unsigned long p;

	p = len << VIRTIO_XSK_FLAG_OFFSET;

	return (void *)(p | VIRTIO_XSK_FLAG);
}

static inline u32 virtnet_ptr_to_xsk(void *ptr)
{
	return ((unsigned long)ptr) >> VIRTIO_XSK_FLAG_OFFSET;
}

int virtnet_xsk_pool_setup(struct net_device *dev, struct netdev_bpf *xdp);
bool virtnet_xsk_xmit(struct virtnet_sq *sq, struct xsk_buff_pool *pool,
		      int budget);
int virtnet_xsk_wakeup(struct net_device *dev, u32 qid, u32 flag);
int virtnet_add_recvbuf_xsk(struct virtnet_info *vi, struct virtnet_rq *rq,
			    struct xsk_buff_pool *pool, gfp_t gfp);
#endif
