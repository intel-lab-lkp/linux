/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __VIRTIO_NET_H__
#define __VIRTIO_NET_H__

#include <linux/ethtool.h>
#include <linux/average.h>
#include <linux/virtio_net.h>
#include <net/xdp_sock_drv.h>

#define VIRTIO_XDP_FLAG	BIT(0)
#define VIRTIO_XSK_FLAG	BIT(1)
#define VIRTIO_XMIT_DATA_MASK (VIRTIO_XDP_FLAG | VIRTIO_XSK_FLAG)

/* RX packet size EWMA. The average packet size is used to determine the packet
 * buffer size when refilling RX rings. As the entire RX ring may be refilled
 * at once, the weight is chosen so that the EWMA will be insensitive to short-
 * term, transient changes in packet size.
 */
DECLARE_EWMA(pkt_len, 0, 64)

struct virtnet_stat_desc {
	char desc[ETH_GSTRING_LEN];
	size_t offset;
};

struct virtnet_sq_stats {
	struct u64_stats_sync syncp;
	u64 packets;
	u64 bytes;
	u64 xdp_tx;
	u64 xdp_tx_drops;
	u64 kicks;
	u64 tx_timeouts;
};

struct virtnet_rq_stats {
	struct u64_stats_sync syncp;
	u64 packets;
	u64 bytes;
	u64 drops;
	u64 xdp_packets;
	u64 xdp_tx;
	u64 xdp_redirects;
	u64 xdp_drops;
	u64 kicks;
};

#define VIRTNET_SQ_STAT(m)	offsetof(struct virtnet_sq_stats, m)
#define VIRTNET_RQ_STAT(m)	offsetof(struct virtnet_rq_stats, m)

struct virtnet_interrupt_coalesce {
	u32 max_packets;
	u32 max_usecs;
};

/* The dma information of pages allocated at a time. */
struct virtnet_rq_dma {
	dma_addr_t addr;
	u32 ref;
	u16 len;
	u16 need_sync;
};

struct virtnet_sq_dma {
	struct virtnet_sq_dma *next;
	dma_addr_t addr;
	u32 len;
	void *data;
};

struct virtnet_sq_dma_head {
	struct virtnet_sq_dma *free;
	struct virtnet_sq_dma *head;
};

/* Internal representation of a send virtqueue */
struct virtnet_sq {
	/* Virtqueue associated with this virtnet_sq */
	struct virtqueue *vq;

	/* TX: fragments + linear part + virtio header */
	struct scatterlist sg[MAX_SKB_FRAGS + 2];

	/* Name of the send queue: output.$index */
	char name[16];

	struct virtnet_sq_stats stats;

	struct virtnet_interrupt_coalesce intr_coal;

	struct napi_struct napi;

	/* Record whether sq is in reset state. */
	bool reset;

	bool do_dma;

	struct virtnet_sq_dma_head dmainfo;
	struct {
		struct xsk_buff_pool __rcu *pool;

		dma_addr_t hdr_dma_address;

		u32 last_cpu;
		struct __call_single_data csd;

		/* The lock to prevent the repeat of calling
		 * smp_call_function_single_async().
		 */
		spinlock_t ipi_lock;
	} xsk;
};

/* Internal representation of a receive virtqueue */
struct virtnet_rq {
	/* Virtqueue associated with this virtnet_rq */
	struct virtqueue *vq;

	struct napi_struct napi;

	struct bpf_prog __rcu *xdp_prog;

	struct virtnet_rq_stats stats;

	struct virtnet_interrupt_coalesce intr_coal;

	/* Chain pages by the private ptr. */
	struct page *pages;

	/* Average packet length for mergeable receive buffers. */
	struct ewma_pkt_len mrg_avg_pkt_len;

	/* Page frag for packet buffer allocation. */
	struct page_frag alloc_frag;

	/* RX: fragments + linear part + virtio header */
	struct scatterlist sg[MAX_SKB_FRAGS + 2];

	/* Min single buffer size for mergeable buffers case. */
	unsigned int min_buf_len;

	/* Name of this receive queue: input.$index */
	char name[16];

	struct xdp_rxq_info xdp_rxq;

	/* Record the last dma info to free after new pages is allocated. */
	struct virtnet_rq_dma *last_dma;

	/* Do dma by self */
	bool do_dma;

	struct {
		struct xsk_buff_pool __rcu *pool;

		/* xdp rxq used by xsk */
		struct xdp_rxq_info xdp_rxq;
	} xsk;
};

struct virtnet_info {
	struct virtio_device *vdev;
	struct virtqueue *cvq;
	struct net_device *dev;
	struct virtnet_sq *sq;
	struct virtnet_rq *rq;
	unsigned int status;

	/* Max # of queue pairs supported by the device */
	u16 max_queue_pairs;

	/* # of queue pairs currently used by the driver */
	u16 curr_queue_pairs;

	/* # of XDP queue pairs currently used by the driver */
	u16 xdp_queue_pairs;

	/* xdp_queue_pairs may be 0, when xdp is already loaded. So add this. */
	bool xdp_enabled;

	/* I like... big packets and I cannot lie! */
	bool big_packets;

	/* number of sg entries allocated for big packets */
	unsigned int big_packets_num_skbfrags;

	/* Host will merge rx buffers for big packets (shake it! shake it!) */
	bool mergeable_rx_bufs;

	/* Host supports rss and/or hash report */
	bool has_rss;
	bool has_rss_hash_report;
	u8 rss_key_size;
	u16 rss_indir_table_size;
	u32 rss_hash_types_supported;
	u32 rss_hash_types_saved;

	/* Has control virtqueue */
	bool has_cvq;

	/* Host can handle any s/g split between our header and packet data */
	bool any_header_sg;

	/* Packet virtio header size */
	u8 hdr_len;

	/* Work struct for delayed refilling if we run low on memory. */
	struct delayed_work refill;

	/* Is delayed refill enabled? */
	bool refill_enabled;

	/* The lock to synchronize the access to refill_enabled */
	spinlock_t refill_lock;

	/* Work struct for config space updates */
	struct work_struct config_work;

	/* Does the affinity hint is set for virtqueues? */
	bool affinity_hint_set;

	/* CPU hotplug instances for online & dead */
	struct hlist_node node;
	struct hlist_node node_dead;

	struct control_buf *ctrl;

	/* Ethtool settings */
	u8 duplex;
	u32 speed;

	/* Interrupt coalescing settings */
	struct virtnet_interrupt_coalesce intr_coal_tx;
	struct virtnet_interrupt_coalesce intr_coal_rx;

	unsigned long guest_offloads;
	unsigned long guest_offloads_capable;

	/* failover when STANDBY feature enabled */
	struct failover *failover;
};

#include "xsk.h"

static inline bool virtnet_is_xdp_frame(void *ptr)
{
	return (unsigned long)ptr & VIRTIO_XDP_FLAG;
}

static inline struct xdp_frame *virtnet_ptr_to_xdp(void *ptr)
{
	return (struct xdp_frame *)((unsigned long)ptr & ~VIRTIO_XDP_FLAG);
}

static inline void *virtnet_sq_unmap(struct virtnet_sq *sq, void *data)
{
	struct virtnet_sq_dma *next, *head;

	head = (void *)((u64)data & ~VIRTIO_XMIT_DATA_MASK);

	data = head->data;

	while (head) {
		virtqueue_dma_unmap_page_attrs(sq->vq, head->addr, head->len, DMA_TO_DEVICE, 0);

		next = head->next;

		head->next = sq->dmainfo.free;
		sq->dmainfo.free = head;

		head = next;
	}

	return data;
}

static inline void virtnet_free_old_xmit(struct virtnet_sq *sq, bool in_napi,
					 struct virtnet_sq_stats *stats)
{
	unsigned int len;
	void *ptr;

	while ((ptr = virtqueue_get_buf(sq->vq, &len)) != NULL) {
		if (!virtnet_is_xdp_frame(ptr)) {
			struct sk_buff *skb;

			if (sq->do_dma)
				ptr = virtnet_sq_unmap(sq, ptr);

			skb = ptr;

			pr_debug("Sent skb %p\n", skb);

			stats->bytes += skb->len;
			napi_consume_skb(skb, in_napi);
		} else {
			struct xdp_frame *frame;

			if (sq->do_dma)
				ptr = virtnet_sq_unmap(sq, ptr);

			frame = virtnet_ptr_to_xdp(ptr);

			stats->bytes += xdp_get_frame_len(frame);
			xdp_return_frame(frame);
		}
		stats->packets++;
	}
}

static inline void virtnet_vq_napi_schedule(struct napi_struct *napi,
					    struct virtqueue *vq)
{
	if (napi_schedule_prep(napi)) {
		virtqueue_disable_cb(vq);
		__napi_schedule(napi);
	}
}

static inline bool virtnet_is_xdp_raw_buffer_queue(struct virtnet_info *vi, int q)
{
	if (q < (vi->curr_queue_pairs - vi->xdp_queue_pairs))
		return false;
	else if (q < vi->curr_queue_pairs)
		return true;
	else
		return false;
}

void virtnet_rx_pause(struct virtnet_info *vi, struct virtnet_rq *rq);
void virtnet_rx_resume(struct virtnet_info *vi, struct virtnet_rq *rq);
void virtnet_tx_pause(struct virtnet_info *vi, struct virtnet_sq *sq);
void virtnet_tx_resume(struct virtnet_info *vi, struct virtnet_sq *sq);
int virtnet_sq_set_premapped(struct virtnet_sq *sq);
void virtnet_sq_unset_premapped(struct virtnet_sq *sq);
void virtnet_rq_free_unused_buf(struct virtqueue *vq, void *buf);
void virtnet_sq_free_unused_buf(struct virtqueue *vq, void *buf);
#endif
