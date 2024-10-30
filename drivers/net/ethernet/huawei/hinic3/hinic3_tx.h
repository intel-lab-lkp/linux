/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_TX_H
#define HINIC3_TX_H

#include <net/ipv6.h>
#include <net/checksum.h>
#include <net/ip6_checksum.h>
#include <linux/ip.h>
#include <linux/ipv6.h>

#include "hinic3_nic_io.h"

#define VXLAN_OFFLOAD_PORT_LE           cpu_to_be16(4789)

#define COMPACET_WQ_SKB_MAX_LEN         16383

#define IP_HDR_IHL_UNIT_SHIFT           2
#define TCP_HDR_DATA_OFF_UNIT_SHIFT     2
#define TCP_HDR_DOFF_UNIT               2
#define TRANSPORT_OFFSET(l4_hdr, skb)   ((u32)((l4_hdr) - (skb)->data))

enum tx_offload_type {
	TX_OFFLOAD_TSO     = BIT(0),
	TX_OFFLOAD_CSUM    = BIT(1),
	TX_OFFLOAD_VLAN    = BIT(2),
	TX_OFFLOAD_INVALID = BIT(3),
	TX_OFFLOAD_ESP     = BIT(4),
};

enum sq_wqe_data_format {
	SQ_NORMAL_WQE = 0,
};

enum sq_wqe_ec_type {
	SQ_WQE_COMPACT_TYPE  = 0,
	SQ_WQE_EXTENDED_TYPE = 1,
};

enum sq_wqe_tasksect_len_type {
	SQ_WQE_TASKSECT_46BITS  = 0,
	SQ_WQE_TASKSECT_16BYTES = 1,
};

#define SQ_CTRL_BUFDESC_NUM_MASK   GENMASK(26, 19)
#define SQ_CTRL_TASKSECT_LEN_MASK  BIT(27)
#define SQ_CTRL_DATA_FORMAT_MASK   BIT(28)
#define SQ_CTRL_EXTENDED_MASK      BIT(30)
#define SQ_CTRL_OWNER_MASK         BIT(31)
#define SQ_CTRL_SET(val, member) \
	FIELD_PREP(SQ_CTRL_##member##_MASK, val)

#define SQ_CTRL_QUEUE_INFO_PLDOFF_MASK  GENMASK(9, 2)
#define SQ_CTRL_QUEUE_INFO_UFO_MASK     BIT(10)
#define SQ_CTRL_QUEUE_INFO_TSO_MASK     BIT(11)
#define SQ_CTRL_QUEUE_INFO_MSS_MASK     GENMASK(26, 13)
#define SQ_CTRL_QUEUE_INFO_UC_MASK      BIT(28)

#define SQ_CTRL_QUEUE_INFO_SET(val, member) \
	FIELD_PREP(SQ_CTRL_QUEUE_INFO_##member##_MASK, val)
#define SQ_CTRL_QUEUE_INFO_GET(val, member) \
	FIELD_GET(SQ_CTRL_QUEUE_INFO_##member##_MASK, val)

#define SQ_TASK_INFO0_TUNNEL_FLAG_MASK  BIT(19)
#define SQ_TASK_INFO0_INNER_L4_EN_MASK  BIT(24)
#define SQ_TASK_INFO0_INNER_L3_EN_MASK  BIT(25)
#define SQ_TASK_INFO0_OUT_L4_EN_MASK    BIT(27)
#define SQ_TASK_INFO0_OUT_L3_EN_MASK    BIT(28)
#define SQ_TASK_INFO0_SET(val, member) \
	FIELD_PREP(SQ_TASK_INFO0_##member##_MASK, val)

#define SQ_TASK_INFO3_VLAN_TAG_MASK        GENMASK(15, 0)
#define SQ_TASK_INFO3_VLAN_TPID_MASK       GENMASK(18, 16)
#define SQ_TASK_INFO3_VLAN_TAG_VALID_MASK  BIT(19)
#define SQ_TASK_INFO3_SET(val, member) \
	FIELD_PREP(SQ_TASK_INFO3_##member##_MASK, val)

struct hinic3_sq_wqe_desc {
	u32 ctrl_len;
	u32 queue_info;
	u32 hi_addr;
	u32 lo_addr;
};

struct hinic3_sq_task {
	u32 pkt_info0;
	u32 ip_identify;
	u32 rsvd;
	u32 vlan_offload;
};

struct hinic3_sq_wqe_combo {
	struct hinic3_sq_wqe_desc *ctrl_bd0;
	struct hinic3_sq_task     *task;
	struct hinic3_sq_bufdesc  *bds_head;
	struct hinic3_sq_bufdesc  *bds_sec2;
	u16                       first_bds_num;
	u32                       wqe_type;
	u32                       task_type;
};

union hinic3_ip {
	struct iphdr   *v4;
	struct ipv6hdr *v6;
	unsigned char  *hdr;
};

struct hinic3_txq_stats {
	u64                   packets;
	u64                   bytes;
	u64                   busy;
	u64                   wake;
	u64                   dropped;
	u64                   skb_pad_err;
	u64                   frag_len_overflow;
	u64                   offload_cow_skb_err;
	u64                   map_frag_err;
	u64                   unknown_tunnel_pkt;
	u64                   frag_size_err;
	struct u64_stats_sync syncp;
};

struct hinic3_dma_info {
	dma_addr_t dma;
	u32        len;
};

struct hinic3_tx_info {
	struct sk_buff         *skb;

	u16                    wqebb_cnt;

	struct hinic3_dma_info *dma_info;
};

struct hinic3_txq {
	struct net_device       *netdev;
	struct device           *dev;

	u16                     q_id;
	u32                     q_mask;
	u32                     q_depth;
	u32                     rsvd2;

	struct hinic3_tx_info   *tx_info;
	struct hinic3_io_queue  *sq;

	struct hinic3_txq_stats txq_stats;
	u64                     last_moder_packets;
	u64                     last_moder_bytes;
} ____cacheline_aligned;

struct hinic3_dyna_txq_res {
	struct hinic3_tx_info  *tx_info;
	struct hinic3_dma_info *bds;
};

int hinic3_alloc_txqs(struct net_device *netdev);
void hinic3_free_txqs(struct net_device *netdev);

int hinic3_alloc_txqs_res(struct net_device *netdev, u16 num_sq,
			  u32 sq_depth, struct hinic3_dyna_txq_res *txqs_res);
void hinic3_free_txqs_res(struct net_device *netdev, u16 num_sq,
			  u32 sq_depth, struct hinic3_dyna_txq_res *txqs_res);
int hinic3_configure_txqs(struct net_device *netdev, u16 num_sq,
			  u32 sq_depth, struct hinic3_dyna_txq_res *txqs_res);

static inline __sum16 csum_magic(union hinic3_ip *ip, unsigned short proto)
{
	return (ip->v4->version == 4) ?
		csum_tcpudp_magic(ip->v4->saddr, ip->v4->daddr, 0, proto, 0) :
		csum_ipv6_magic(&ip->v6->saddr, &ip->v6->daddr, 0, proto, 0);
}

netdev_tx_t hinic3_xmit_frame(struct sk_buff *skb, struct net_device *netdev);
void hinic3_txq_get_stats(struct hinic3_txq *txq,
			  struct hinic3_txq_stats *stats);
int hinic3_tx_poll(struct hinic3_txq *txq, int budget);
int hinic3_flush_txqs(struct net_device *netdev);

#endif
