/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __VIRTIO_NET_H__
#define __VIRTIO_NET_H__

#include <linux/ethtool.h>
#include <linux/average.h>

/* RX packet size EWMA. The average packet size is used to determine the packet
 * buffer size when refilling RX rings. As the entire RX ring may be refilled
 * at once, the weight is chosen so that the EWMA will be insensitive to short-
 * term, transient changes in packet size.
 */
DECLARE_EWMA(pkt_len, 0, 64)

struct virtnet_sq_stats {
	struct u64_stats_sync syncp;
	u64_stats_t packets;
	u64_stats_t bytes;
	u64_stats_t xdp_tx;
	u64_stats_t xdp_tx_drops;
	u64_stats_t kicks;
	u64_stats_t tx_timeouts;
	u64_stats_t stop;
	u64_stats_t wake;
};

struct virtnet_rq_stats {
	struct u64_stats_sync syncp;
	u64_stats_t packets;
	u64_stats_t bytes;
	u64_stats_t drops;
	u64_stats_t xdp_packets;
	u64_stats_t xdp_tx;
	u64_stats_t xdp_redirects;
	u64_stats_t xdp_drops;
	u64_stats_t kicks;
};

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
};

/* Internal representation of a receive virtqueue */
struct virtnet_rq {
	/* Virtqueue associated with this virtnet_rq */
	struct virtqueue *vq;

	struct napi_struct napi;

	struct bpf_prog __rcu *xdp_prog;

	struct virtnet_rq_stats stats;

	/* The number of rx notifications */
	u16 calls;

	/* Is dynamic interrupt moderation enabled? */
	bool dim_enabled;

	/* Used to protect dim_enabled and inter_coal */
	struct mutex dim_lock;

	/* Dynamic Interrupt Moderation */
	struct dim dim;

	u32 packets_in_napi;

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
};

/* This structure can contain rss message with maximum settings for indirection table and keysize
 * Note, that default structure that describes RSS configuration virtio_net_rss_config
 * contains same info but can't handle table values.
 * In any case, structure would be passed to virtio hw through sg_buf split by parts
 * because table sizes may be differ according to the device configuration.
 */
#define VIRTIO_NET_RSS_MAX_KEY_SIZE     40
#define VIRTIO_NET_RSS_MAX_TABLE_LEN    128
struct virtio_net_ctrl_rss {
	u32 hash_types;
	u16 indirection_table_mask;
	u16 unclassified_queue;
	u16 indirection_table[VIRTIO_NET_RSS_MAX_TABLE_LEN];
	u16 max_tx_vq;
	u8 hash_key_length;
	u8 key[VIRTIO_NET_RSS_MAX_KEY_SIZE];
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
	struct virtio_net_ctrl_rss rss;

	/* Has control virtqueue */
	bool has_cvq;

	/* Lock to protect the control VQ */
	struct mutex cvq_lock;

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

	/* Work struct for setting rx mode */
	struct work_struct rx_mode_work;

	/* OK to queue work setting RX mode? */
	bool rx_mode_work_enabled;

	/* Does the affinity hint is set for virtqueues? */
	bool affinity_hint_set;

	/* CPU hotplug instances for online & dead */
	struct hlist_node node;
	struct hlist_node node_dead;

	struct control_buf *ctrl;

	/* Ethtool settings */
	u8 duplex;
	u32 speed;

	/* Is rx dynamic interrupt moderation enabled? */
	bool rx_dim_enabled;

	/* Interrupt coalescing settings */
	struct virtnet_interrupt_coalesce intr_coal_tx;
	struct virtnet_interrupt_coalesce intr_coal_rx;

	unsigned long guest_offloads;
	unsigned long guest_offloads_capable;

	/* failover when STANDBY feature enabled */
	struct failover *failover;

	u64 device_stats_cap;
};

void virtnet_rx_pause(struct virtnet_info *vi, struct virtnet_rq *rq);
void virtnet_rx_resume(struct virtnet_info *vi, struct virtnet_rq *rq);
void virtnet_tx_pause(struct virtnet_info *vi, struct virtnet_sq *sq);
void virtnet_tx_resume(struct virtnet_info *vi, struct virtnet_sq *sq);
struct sk_buff *virtnet_skb_append_frag(struct sk_buff *head_skb,
					struct sk_buff *curr_skb,
					struct page *page, void *buf,
					int len, int truesize);
#endif
