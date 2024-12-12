/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_NIC_DEV_H
#define HINIC3_NIC_DEV_H

#include <linux/netdevice.h>

#include "hinic3_hw_cfg.h"
#include "hinic3_mgmt_interface.h"

enum hinic3_flags {
	HINIC3_INTF_UP,
	HINIC3_RSS_ENABLE,
	HINIC3_CHANGE_RES_INVALID,
	HINIC3_RSS_DEFAULT_INDIR,
};

#define HINIC3_CHANNEL_RES_VALID(nic_dev) \
	(test_bit(HINIC3_INTF_UP, &(nic_dev)->flags) && \
	 !test_bit(HINIC3_CHANGE_RES_INVALID, &(nic_dev)->flags))

enum hinic3_rss_hash_type {
	HINIC3_RSS_HASH_ENGINE_TYPE_XOR  = 0,
	HINIC3_RSS_HASH_ENGINE_TYPE_TOEP = 1,
};

struct nic_rss_type {
	u8 tcp_ipv6_ext;
	u8 ipv6_ext;
	u8 tcp_ipv6;
	u8 ipv6;
	u8 tcp_ipv4;
	u8 ipv4;
	u8 udp_ipv6;
	u8 udp_ipv4;
};

struct nic_rss_indirect_tbl_set {
	u32 rsvd[4];
	u16 entry[NIC_RSS_INDIR_SIZE];
};

struct hinic3_irq {
	struct net_device  *netdev;
	u16                msix_entry_idx;
	/* provided by OS */
	u32                irq_id;
	char               irq_name[IFNAMSIZ + 16];
	struct napi_struct napi;
	cpumask_t          affinity_mask;
	struct hinic3_txq  *txq;
	struct hinic3_rxq  *rxq;
};

struct hinic3_dyna_txrxq_params {
	u16                        num_qps;
	u32                        sq_depth;
	u32                        rq_depth;

	struct hinic3_dyna_txq_res *txqs_res;
	struct hinic3_dyna_rxq_res *rxqs_res;
	struct hinic3_irq          *irq_cfg;
};

struct hinic3_intr_coal_info {
	u8  pending_limt;
	u8  coalesce_timer_cfg;
	u8  resend_timer_cfg;
};

struct hinic3_nic_dev {
	struct pci_dev                  *pdev;
	struct net_device               *netdev;
	struct hinic3_hwdev             *hwdev;
	struct hinic3_nic_io            *nic_io;

	u16                             max_qps;
	u32                             dma_rx_buff_size;
	u16                             rx_buff_len;
	u32                             page_order;
	u32                             lro_replenish_thld;
	unsigned long                   flags;
	struct nic_service_cap          nic_cap;

	struct hinic3_dyna_txrxq_params q_params;
	struct hinic3_txq               *txqs;
	struct hinic3_rxq               *rxqs;

	enum hinic3_rss_hash_type       rss_hash_type;
	struct nic_rss_type             rss_type;
	u8                              *rss_hkey;
	u32                             *rss_indir;

	u16                             num_qp_irq;
	struct irq_info                 *qps_irq_info;

	struct hinic3_intr_coal_info    *intr_coalesce;

	bool                            link_status_up;
};

void hinic3_set_netdev_ops(struct net_device *netdev);
int hinic3_qps_irq_init(struct net_device *netdev);
void hinic3_qps_irq_deinit(struct net_device *netdev);

#endif
