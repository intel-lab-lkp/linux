/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_RX_H
#define HINIC3_RX_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/u64_stats_sync.h>
#include <linux/bits.h>
#include <linux/bitfield.h>

#include "hinic3_nic_io.h"
#include "hinic3_nic_dev.h"

/* rx cqe checksum err */
#define HINIC3_RX_CSUM_IP_CSUM_ERR      BIT(0)
#define HINIC3_RX_CSUM_TCP_CSUM_ERR     BIT(1)
#define HINIC3_RX_CSUM_UDP_CSUM_ERR     BIT(2)
#define HINIC3_RX_CSUM_IGMP_CSUM_ERR    BIT(3)
#define HINIC3_RX_CSUM_ICMPV4_CSUM_ERR  BIT(4)
#define HINIC3_RX_CSUM_ICMPV6_CSUM_ERR  BIT(5)
#define HINIC3_RX_CSUM_SCTP_CRC_ERR     BIT(6)
#define HINIC3_RX_CSUM_HW_CHECK_NONE    BIT(7)
#define HINIC3_RX_CSUM_IPSU_OTHER_ERR   BIT(8)

#define HINIC3_HEADER_DATA_UNIT         2

#define RQ_CQE_OFFOLAD_TYPE_PKT_TYPE_MASK           GENMASK(4, 0)
#define RQ_CQE_OFFOLAD_TYPE_IP_TYPE_MASK            GENMASK(6, 5)
#define RQ_CQE_OFFOLAD_TYPE_TUNNEL_PKT_FORMAT_MASK  GENMASK(11, 8)
#define RQ_CQE_OFFOLAD_TYPE_VLAN_EN_MASK            BIT(21)
#define RQ_CQE_OFFOLAD_TYPE_GET(val, member) \
	FIELD_GET(RQ_CQE_OFFOLAD_TYPE_##member##_MASK, val)

#define HINIC3_GET_RX_PKT_TYPE(offload_type) \
	RQ_CQE_OFFOLAD_TYPE_GET(offload_type, PKT_TYPE)
#define HINIC3_GET_RX_IP_TYPE(offload_type) \
	RQ_CQE_OFFOLAD_TYPE_GET(offload_type, IP_TYPE)
#define HINIC3_GET_RX_TUNNEL_PKT_FORMAT(offload_type) \
	RQ_CQE_OFFOLAD_TYPE_GET(offload_type, TUNNEL_PKT_FORMAT)
#define HINIC3_GET_RX_VLAN_OFFLOAD_EN(offload_type) \
	RQ_CQE_OFFOLAD_TYPE_GET(offload_type, VLAN_EN)

#define RQ_CQE_SGE_VLAN_MASK  GENMASK(15, 0)
#define RQ_CQE_SGE_LEN_MASK   GENMASK(31, 16)
#define RQ_CQE_SGE_GET(val, member) \
	FIELD_GET(RQ_CQE_SGE_##member##_MASK, val)

#define HINIC3_GET_RX_VLAN_TAG(vlan_len)  RQ_CQE_SGE_GET(vlan_len, VLAN)
#define HINIC3_GET_RX_PKT_LEN(vlan_len)   RQ_CQE_SGE_GET(vlan_len, LEN)

#define RQ_CQE_STATUS_CSUM_ERR_MASK  GENMASK(15, 0)
#define RQ_CQE_STATUS_NUM_LRO_MASK   GENMASK(23, 16)
#define RQ_CQE_STATUS_RXDONE_MASK    BIT(31)
#define RQ_CQE_STATUS_GET(val, member) \
	FIELD_GET(RQ_CQE_STATUS_##member##_MASK, val)

#define HINIC3_GET_RX_CSUM_ERR(status)  RQ_CQE_STATUS_GET(status, CSUM_ERR)
#define HINIC3_GET_RX_DONE(status)      RQ_CQE_STATUS_GET(status, RXDONE)
#define HINIC3_GET_RX_NUM_LRO(status)   RQ_CQE_STATUS_GET(status, NUM_LRO)

struct hinic3_rxq_stats {
	u64                   packets;
	u64                   bytes;
	u64                   errors;
	u64                   csum_errors;
	u64                   other_errors;
	u64                   dropped;
	u64                   rx_buf_empty;
	u64                   alloc_skb_err;
	u64                   alloc_rx_buf_err;
	u64                   restore_drop_sge;
	struct u64_stats_sync syncp;
};

/* RX Completion information that is provided by HW for a specific RX WQE */
struct hinic3_rq_cqe {
	u32 status;
	u32 vlan_len;
	u32 offload_type;
	u32 rsvd3;
	u32 rsvd4;
	u32 rsvd5;
	u32 rsvd6;
	u32 pkt_info;
};

struct hinic3_rq_wqe {
	u32 buf_hi_addr;
	u32 buf_lo_addr;
	u32 cqe_hi_addr;
	u32 cqe_lo_addr;
};

struct hinic3_rx_info {
	dma_addr_t  buf_dma_addr;
	struct page *page;
	u32         page_offset;
};

struct hinic3_rxq {
	struct net_device       *netdev;

	u16                     q_id;
	u32                     q_depth;
	u32                     q_mask;

	u16                     buf_len;
	u32                     rx_buff_shift;
	u32                     dma_rx_buff_size;

	struct hinic3_rxq_stats rxq_stats;
	u32                     cons_idx;
	u32                     delta;

	u32                     irq_id;
	u16                     msix_entry_idx;

	/* cqe_arr and rx_info are arrays of rq_depth elements. Each element is
	 * statically associated (by index) to a specific rq_wqe.
	 */
	struct hinic3_rq_cqe   *cqe_arr;
	struct hinic3_rx_info  *rx_info;

	struct hinic3_io_queue *rq;

	struct hinic3_irq      *irq_cfg;
	u16                    next_to_alloc;
	u16                    next_to_update;
	struct device          *dev; /* device for DMA mapping */

	dma_addr_t             cqe_start_paddr;

	u64                    last_moder_packets;
	u64                    last_moder_bytes;
	u8                     last_coalesc_timer_cfg;
	u8                     last_pending_limt;
	u16                    restore_buf_num;

	u32                    last_sw_pi;
	u32                    last_sw_ci;

	u16                    last_hw_ci;
	u8                     rx_check_err_cnt;
	u8                     rxq_print_times;
	u32                    restore_pi;

	u64                    last_packets;
} ____cacheline_aligned;

struct hinic3_dyna_rxq_res {
	u16                   next_to_alloc;
	struct hinic3_rx_info *rx_info;
	dma_addr_t            cqe_start_paddr;
	void                  *cqe_start_vaddr;
};

int hinic3_alloc_rxqs(struct net_device *netdev);
void hinic3_free_rxqs(struct net_device *netdev);

int hinic3_alloc_rxqs_res(struct net_device *netdev, u16 num_rq,
			  u32 rq_depth, struct hinic3_dyna_rxq_res *rxqs_res);
void hinic3_free_rxqs_res(struct net_device *netdev, u16 num_rq,
			  u32 rq_depth, struct hinic3_dyna_rxq_res *rxqs_res);
int hinic3_configure_rxqs(struct net_device *netdev, u16 num_rq,
			  u32 rq_depth, struct hinic3_dyna_rxq_res *rxqs_res);

void hinic3_rxq_get_stats(struct hinic3_rxq *rxq,
			  struct hinic3_rxq_stats *stats);
int hinic3_rx_poll(struct hinic3_rxq *rxq, int budget);

#endif
