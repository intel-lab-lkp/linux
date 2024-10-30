/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC_MGMT_INTERFACE_H
#define HINIC_MGMT_INTERFACE_H

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/if_ether.h>

#define HINIC3_CMD_OP_SET  1
#define HINIC3_CMD_OP_GET  0

enum nic_feature_cap {
	NIC_F_CSUM           = BIT(0),
	NIC_F_SCTP_CRC       = BIT(1),
	NIC_F_TSO            = BIT(2),
	NIC_F_LRO            = BIT(3),
	NIC_F_UFO            = BIT(4),
	NIC_F_RSS            = BIT(5),
	NIC_F_RX_VLAN_FILTER = BIT(6),
	NIC_F_RX_VLAN_STRIP  = BIT(7),
	NIC_F_TX_VLAN_INSERT = BIT(8),
	NIC_F_VXLAN_OFFLOAD  = BIT(9),
	NIC_F_FDIR           = BIT(11),
	NIC_F_PROMISC        = BIT(12),
	NIC_F_ALLMULTI       = BIT(13),
	NIC_F_RATE_LIMIT     = BIT(16),
};

#define NIC_F_ALL_MASK           0x33bff
#define NIC_DRV_DEFAULT_FEATURE  0x3f03f

struct hinic3_mgmt_msg_head {
	u8 status;
	u8 version;
	u8 rsvd0[6];
};

#define NIC_MAX_FEATURE_QWORD  4

struct hinic3_cmd_feature_nego {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	/* 1: set, 0: get */
	u8                          opcode;
	u8                          rsvd;
	u64                         s_feature[NIC_MAX_FEATURE_QWORD];
};

enum hinic3_func_tbl_cfg_bitmap {
	FUNC_CFG_INIT        = 0,
	FUNC_CFG_RX_BUF_SIZE = 1,
	FUNC_CFG_MTU         = 2,
};

struct hinic3_func_tbl_cfg {
	u16 rx_wqe_buf_size;
	u16 mtu;
	u32 rsvd[9];
};

struct hinic3_cmd_set_func_tbl {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         rsvd;

	u32                         cfg_bitmap;
	struct hinic3_func_tbl_cfg  tbl_cfg;
};

struct hinic3_port_mac_set {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         vlan_id;
	u16                         rsvd1;
	u8                          mac[ETH_ALEN];
};

struct hinic3_port_mac_update {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         vlan_id;
	u16                         rsvd1;
	u8                          old_mac[ETH_ALEN];
	u16                         rsvd2;
	u8                          new_mac[ETH_ALEN];
};

struct hinic3_cmd_vlan_config {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u8                          opcode;
	u8                          rsvd1;
	u16                         vlan_id;
	u16                         rsvd2;
};

struct hinic3_cmd_vlan_offload {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u8                          vlan_offload;
	u8                          vd1[5];
};

/* set vlan filter */
struct hinic3_cmd_set_vlan_filter {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u8                          resvd[2];
	/* bit0:vlan filter en; bit1:broadcast_filter_en */
	u32                         vlan_filter_ctrl;
};

struct hinic3_cmd_cons_idx_attr {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_idx;
	u8                          dma_attr_off;
	u8                          pending_limit;
	u8                          coalescing_time;
	u8                          intr_en;
	u16                         intr_idx;
	u32                         l2nic_sqn;
	u32                         rsvd;
	u64                         ci_addr;
};

struct hinic3_cmd_clear_qp_resource {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         rsvd1;
};

struct hinic3_force_pkt_drop {
	struct hinic3_mgmt_msg_head msg_head;

	u8                          port;
	u8                          rsvd1[3];
};

struct hinic3_vport_state {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         rsvd1;
	/* 0--disable, 1--enable */
	u8                          state;
	u8                          rsvd2[3];
};

/* *
 * Definition of the NIC receiving mode
 */
#define NIC_RX_MODE_UC       0x01
#define NIC_RX_MODE_MC       0x02
#define NIC_RX_MODE_BC       0x04
#define NIC_RX_MODE_MC_ALL   0x08
#define NIC_RX_MODE_PROMISC  0x10

struct hinic3_rx_mode_config {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         rsvd1;
	u32                         rx_mode;
};

/* IEEE 802.1Qaz std */
#define NIC_DCB_COS_MAX      0x8
#define NIC_DCB_MAX_PFC_NUM  0x4

struct hinic3_cmd_set_dcb_state {
	struct hinic3_mgmt_msg_head head;

	u16                         func_id;
	/* 0 - get dcb state, 1 - set dcb state */
	u8                          op_code;
	/* 0 - disable, 1 - enable dcb */
	u8                          state;
	/* 0 - disable, 1 - enable dcb */
	u8                          port_state;
	u8                          rsvd[7];
};

struct hinic3_cmd_pause_config {
	struct hinic3_mgmt_msg_head msg_head;

	u8                          port_id;
	u8                          opcode;
	u16                         rsvd1;
	u8                          auto_neg;
	u8                          rx_pause;
	u8                          tx_pause;
	u8                          rsvd2[5];
};

struct hinic3_port_stats_info {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         rsvd1;
};

struct hinic3_vport_stats {
	u64 tx_unicast_pkts_vport;
	u64 tx_unicast_bytes_vport;
	u64 tx_multicast_pkts_vport;
	u64 tx_multicast_bytes_vport;
	u64 tx_broadcast_pkts_vport;
	u64 tx_broadcast_bytes_vport;

	u64 rx_unicast_pkts_vport;
	u64 rx_unicast_bytes_vport;
	u64 rx_multicast_pkts_vport;
	u64 rx_multicast_bytes_vport;
	u64 rx_broadcast_pkts_vport;
	u64 rx_broadcast_bytes_vport;

	u64 tx_discard_vport;
	u64 rx_discard_vport;
	u64 tx_err_vport;
	u64 rx_err_vport;
};

struct hinic3_cmd_vport_stats {
	struct hinic3_mgmt_msg_head msg_head;

	u32                         stats_size;
	u32                         rsvd1;
	struct hinic3_vport_stats   stats;
	u64                         rsvd2[6];
};

struct hinic3_cmd_lro_config {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u8                          opcode;
	u8                          rsvd1;
	u8                          lro_ipv4_en;
	u8                          lro_ipv6_en;
	/* unit is 1K */
	u8                          lro_max_pkt_len;
	u8                          resv2[13];
};

struct hinic3_cmd_lro_timer {
	struct hinic3_mgmt_msg_head msg_head;

	/* 1: set timer value, 0: get timer value */
	u8                          opcode;
	u8                          rsvd[3];
	u32                         timer;
};

#define HINIC3_RSS_TYPE_VALID_MASK         BIT(23)
#define HINIC3_RSS_TYPE_TCP_IPV6_EXT_MASK  BIT(24)
#define HINIC3_RSS_TYPE_IPV6_EXT_MASK      BIT(25)
#define HINIC3_RSS_TYPE_TCP_IPV6_MASK      BIT(26)
#define HINIC3_RSS_TYPE_IPV6_MASK          BIT(27)
#define HINIC3_RSS_TYPE_TCP_IPV4_MASK      BIT(28)
#define HINIC3_RSS_TYPE_IPV4_MASK          BIT(29)
#define HINIC3_RSS_TYPE_UDP_IPV6_MASK      BIT(30)
#define HINIC3_RSS_TYPE_UDP_IPV4_MASK      BIT(31)
#define HINIC3_RSS_TYPE_SET(val, member)  \
	FIELD_PREP(HINIC3_RSS_TYPE_##member##_MASK, val)
#define HINIC3_RSS_TYPE_GET(val, member)  \
	FIELD_GET(HINIC3_RSS_TYPE_##member##_MASK, val)

enum nic_rss_hash_type {
	NIC_RSS_HASH_TYPE_XOR  = 0,
	NIC_RSS_HASH_TYPE_TOEP = 1,

	/* must be the last one */
	NIC_RSS_HASH_TYPE_MAX
};

#define NIC_RSS_INDIR_SIZE  256
#define NIC_RSS_KEY_SIZE    40

struct hinic3_rss_context_table {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u16                         rsvd1;
	u32                         context;
};

struct hinic3_cmd_rss_engine_type {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u8                          opcode;
	u8                          hash_engine;
	u8                          rsvd1[4];
};

struct hinic3_cmd_rss_hash_key {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u8                          opcode;
	u8                          rsvd1;
	u8                          key[NIC_RSS_KEY_SIZE];
};

struct hinic3_cmd_rss_config {
	struct hinic3_mgmt_msg_head msg_head;

	u16                         func_id;
	u8                          rss_en;
	u8                          rq_priority_number;
	u8                          prio_tc[NIC_DCB_COS_MAX];
	u16                         num_qps;
	u16                         rsvd1;
};

/* Commands between NIC to fw */
enum hinic3_nic_cmd {
	/* FUNC CFG */
	HINIC3_NIC_CMD_SET_FUNC_TBL              = 5,
	HINIC3_NIC_CMD_SET_VPORT_ENABLE          = 6,
	HINIC3_NIC_CMD_SET_RX_MODE               = 7,
	HINIC3_NIC_CMD_SQ_CI_ATTR_SET            = 8,
	HINIC3_NIC_CMD_GET_VPORT_STAT            = 9,
	HINIC3_NIC_CMD_CLEAN_VPORT_STAT          = 10,
	HINIC3_NIC_CMD_CLEAR_QP_RESOURCE         = 11,
	HINIC3_NIC_CMD_CFG_FLEX_QUEUE            = 12,
	/* LRO CFG */
	HINIC3_NIC_CMD_CFG_RX_LRO                = 13,
	HINIC3_NIC_CMD_CFG_LRO_TIMER             = 14,
	HINIC3_NIC_CMD_FEATURE_NEGO              = 15,
	HINIC3_NIC_CMD_CFG_LOCAL_LRO_STATE       = 16,

	/* MAC & VLAN CFG */
	HINIC3_NIC_CMD_GET_MAC                   = 20,
	HINIC3_NIC_CMD_SET_MAC                   = 21,
	HINIC3_NIC_CMD_DEL_MAC                   = 22,
	HINIC3_NIC_CMD_UPDATE_MAC                = 23,
	HINIC3_NIC_CMD_GET_ALL_DEFAULT_MAC       = 24,

	HINIC3_NIC_CMD_CFG_FUNC_VLAN             = 25,
	HINIC3_NIC_CMD_SET_VLAN_FILTER_EN        = 26,
	HINIC3_NIC_CMD_SET_RX_VLAN_OFFLOAD       = 27,

	HINIC3_NIC_CMD_RSS_CFG                   = 60,

	HINIC3_NIC_CMD_GET_RSS_CTX_TBL           = 62,
	HINIC3_NIC_CMD_CFG_RSS_HASH_KEY          = 63,
	HINIC3_NIC_CMD_CFG_RSS_HASH_ENGINE       = 64,
	HINIC3_NIC_CMD_SET_RSS_CTX_TBL_INTO_FUNC = 65,

	HINIC3_NIC_CMD_SET_PORT_ENABLE           = 100,
	HINIC3_NIC_CMD_CFG_PAUSE_INFO            = 101,
	HINIC3_NIC_CMD_SET_PORT_CAR              = 102,
	HINIC3_NIC_CMD_SET_ER_DROP_PKT           = 103,
	HINIC3_NIC_CMD_VF_COS                    = 104,
	HINIC3_NIC_CMD_SETUP_COS_MAPPING         = 105,
	HINIC3_NIC_CMD_SET_ETS                   = 106,
	HINIC3_NIC_CMD_SET_PFC                   = 107,
	HINIC3_NIC_CMD_QOS_ETS                   = 108,
	HINIC3_NIC_CMD_QOS_PFC                   = 109,
	HINIC3_NIC_CMD_QOS_DCB_STATE             = 110,
	HINIC3_NIC_CMD_QOS_PORT_CFG              = 111,
	HINIC3_NIC_CMD_QOS_MAP_CFG               = 112,
	HINIC3_NIC_CMD_FORCE_PKT_DROP            = 113,

	HINIC3_NIC_CMD_MAX                       = 256,
};

/* NIC CMDQ MODE */
enum hinic3_ucode_cmd {
	HINIC3_UCODE_CMD_MODIFY_QUEUE_CTX    = 0,
	HINIC3_UCODE_CMD_CLEAN_QUEUE_CONTEXT = 1,
	HINIC3_UCODE_CMD_SET_RSS_INDIR_TABLE = 4,
	HINIC3_UCODE_CMD_GET_RSS_INDIR_TABLE = 6,
	HINIC3_UCODE_CMD_MODIFY_VLAN_CTX     = 11,
};

#endif
