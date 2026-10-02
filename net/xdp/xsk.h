/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2019 Intel Corporation. */

#ifndef XSK_H_
#define XSK_H_

#include <net/netmem.h>

struct xsk_buff_pool;

enum xsk_pool_clear_mode {
	XSK_POOL_CLEAR_NORMAL,
	XSK_POOL_CLEAR_FORCE,
};

struct xsk_pp_info {
	struct net_iov_area area;
	struct xsk_buff_pool *pool;
	u32 chunk_shift;
};

/* Keep page_pool descriptors independent from direct-driver XSK buffers.
 * Queue replacement creates the new page_pool before it stops the old queue
 * and destroys the old page_pool after the new queue started, so two
 * page_pools can use one provider. @lock serializes the state they share.
 * Generic XDP cannot deliver to a provider-backed socket.
 */
struct xsk_pp {
	struct xsk_pp_info info;
	struct xsk_queue __rcu *fq;
	/* FILL consumers, @reuse and RX need_wakeup state */
	spinlock_t lock;
	u32 reuse_cnt;
	u32 nr_pools;
	u64 chunk_mask;
	u64 addrs_cnt;
	u8 release_retries;
	bool dma_need_sync;
	bool detached;
	u32 reuse[];
};

void xp_clear_dev(struct xsk_buff_pool *pool, enum xsk_pool_clear_mode mode);

static inline bool xp_netmem_is_xsk(netmem_ref netmem)
{
	return netmem_is_net_iov(netmem) &&
	       netmem_to_net_iov(netmem)->type == NET_IOV_XSK;
}

static inline struct xsk_pp_info *xp_netmem_to_pp(netmem_ref netmem)
{
	struct net_iov *niov = netmem_to_net_iov(netmem);

	return container_of(net_iov_owner(niov), struct xsk_pp_info, area);
}

static inline bool xp_netmem_is_from_pool(netmem_ref netmem,
					  const struct xsk_buff_pool *pool)
{
	return xp_netmem_is_xsk(netmem) &&
	       xp_netmem_to_pp(netmem)->pool == pool;
}

struct xdp_ring_offset_v1 {
	__u64 producer;
	__u64 consumer;
	__u64 desc;
};

struct xdp_mmap_offsets_v1 {
	struct xdp_ring_offset_v1 rx;
	struct xdp_ring_offset_v1 tx;
	struct xdp_ring_offset_v1 fr;
	struct xdp_ring_offset_v1 cr;
};

/* Nodes are linked in the struct xdp_sock map_list field, and used to
 * track which maps a certain socket reside in.
 */

struct xsk_map_node {
	struct list_head node;
	struct xsk_map *map;
	struct xdp_sock __rcu **map_entry;
};

static inline struct xdp_sock *xdp_sk(struct sock *sk)
{
	return (struct xdp_sock *)sk;
}

void xsk_map_try_sock_delete(struct xsk_map *map, struct xdp_sock *xs,
			     struct xdp_sock __rcu **map_entry);
void xsk_clear_pool_at_qid(struct net_device *dev, u16 queue_id);
int xsk_reg_pool_at_qid(struct net_device *dev, struct xsk_buff_pool *pool,
			u16 queue_id);

#endif /* XSK_H_ */
