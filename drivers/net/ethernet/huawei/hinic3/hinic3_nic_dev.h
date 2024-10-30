/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_NIC_DEV_H
#define HINIC3_NIC_DEV_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/semaphore.h>
#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>

#include "hinic3_hw_cfg.h"
#include "hinic3_hwdev.h"
#include "hinic3_mgmt_interface.h"
#include "hinic3_nic_io.h"
#include "hinic3_tx.h"
#include "hinic3_rx.h"

#define VLAN_BITMAP_BYTE_SIZE(nic_dev)  (sizeof(*(nic_dev)->vlan_bitmap))
#define VLAN_BITMAP_SIZE(nic_dev) \
	(VLAN_N_VID / VLAN_BITMAP_BYTE_SIZE(nic_dev))

#define HINIC3_MODERATONE_DELAY  HZ

enum hinic3_flags {
	HINIC3_INTF_UP,
	HINIC3_MAC_FILTER_CHANGED,
	HINIC3_RSS_ENABLE,
	HINIC3_INTR_ADAPT,
	HINIC3_UPDATE_MAC_FILTER,
	HINIC3_CHANGE_RES_INVALID,
	HINIC3_RSS_DEFAULT_INDIR,
};

#define HINIC3_CHANNEL_RES_VALID(nic_dev) \
	(test_bit(HINIC3_INTF_UP, &(nic_dev)->flags) && \
	 !test_bit(HINIC3_CHANGE_RES_INVALID, &(nic_dev)->flags))

enum hinic3_event_work_flags {
	EVENT_WORK_TX_TIMEOUT,
};

#define HINIC3_NIC_STATS_INC(nic_dev, field) \
do { \
	u64_stats_update_begin(&(nic_dev)->stats.syncp); \
	(nic_dev)->stats.field++; \
	u64_stats_update_end(&(nic_dev)->stats.syncp); \
} while (0)

struct hinic3_nic_stats {
	u64                   netdev_tx_timeout;

	/* Subdivision statistics show in private tool */
	u64                   tx_carrier_off_drop;
	u64                   tx_invalid_qid;
	struct u64_stats_sync syncp;
};

enum hinic3_rx_mode_state {
	HINIC3_HW_PROMISC_ON,
	HINIC3_HW_ALLMULTI_ON,
	HINIC3_PROMISC_FORCE_ON,
	HINIC3_ALLMULTI_FORCE_ON,
};

enum mac_filter_state {
	HINIC3_MAC_WAIT_HW_SYNC,
	HINIC3_MAC_HW_SYNCED,
	HINIC3_MAC_WAIT_HW_UNSYNC,
	HINIC3_MAC_HW_UNSYNCED,
};

struct hinic3_mac_filter {
	struct list_head list;
	u8               addr[ETH_ALEN];
	unsigned long    state;
};

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

struct nic_rss_indirect_tbl_get {
	u16 entry[NIC_RSS_INDIR_SIZE];
};

struct hinic3_rx_flow_rule {
	struct list_head rules;
	u32              tot_num_rules;
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

	u64 pkt_rate_low;
	u8  rx_usecs_low;
	u8  rx_pending_limt_low;
	u64 pkt_rate_high;
	u8  rx_usecs_high;
	u8  rx_pending_limt_high;

	u8  user_set_intr_coal_flag;
};

struct hinic3_nic_dev {
	struct pci_dev                  *pdev;
	struct net_device               *netdev;
	struct hinic3_hwdev             *hwdev;
	struct hinic3_nic_io            *nic_io;

	u32                             msg_enable;
	u16                             max_qps;
	u32                             dma_rx_buff_size;
	u16                             rx_buff_len;
	u32                             page_order;
	u32                             lro_replenish_thld;
	unsigned long                   *vlan_bitmap;
	unsigned long                   flags;
	struct nic_service_cap          nic_cap;

	struct hinic3_dyna_txrxq_params q_params;
	struct hinic3_txq               *txqs;
	struct hinic3_rxq               *rxqs;
	struct hinic3_nic_stats         stats;

	enum hinic3_rss_hash_type       rss_hash_type;
	struct nic_rss_type             rss_type;
	u8                              *rss_hkey;
	u32                             *rss_indir;

	u16                             num_qp_irq;
	struct irq_info                 *qps_irq_info;

	struct hinic3_intr_coal_info    *intr_coalesce;
	u32                             adaptive_rx_coal;
	unsigned long                   last_moder_jiffies;

	struct workqueue_struct         *workq;
	struct delayed_work             periodic_work;
	struct delayed_work             moderation_task;
	struct work_struct              rx_mode_work;
	struct semaphore                port_state_sem;

	struct list_head                uc_filter_list;
	struct list_head                mc_filter_list;
	unsigned long                   rx_mod_state;
	int                             netdev_uc_cnt;
	int                             netdev_mc_cnt;

	/* flag bits defined by hinic3_event_work_flags */
	unsigned long                   event_flag;
	bool                            link_status_up;
};

void hinic3_set_netdev_ops(struct net_device *netdev);
int hinic3_set_hw_features(struct net_device *netdev);
int hinic3_change_channel_settings(struct net_device *netdev,
				   struct hinic3_dyna_txrxq_params *trxq_params,
				   void (*reopen_handler)(struct net_device *netdev));

int hinic3_qps_irq_init(struct net_device *netdev);
void hinic3_qps_irq_deinit(struct net_device *netdev);

void hinic3_set_rx_mode_work(struct work_struct *work);
void hinic3_clean_mac_list_filter(struct net_device *netdev);

void hinic3_set_ethtool_ops(struct net_device *netdev);

#endif
