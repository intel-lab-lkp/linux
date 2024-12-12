/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC_MGMT_INTERFACE_H
#define HINIC_MGMT_INTERFACE_H

#include <linux/bits.h>
#include <linux/bitfield.h>
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

struct hinic3_cmd_feature_nego {
	struct hinic3_mgmt_msg_head msg_head;
	u16                         func_id;
	u8                          opcode;
	u8                          rsvd;
	u64                         s_feature[4];
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

struct hinic3_force_pkt_drop {
	struct hinic3_mgmt_msg_head msg_head;
	u8                          port;
	u8                          rsvd1[3];
};

/* Commands between NIC to fw */
enum hinic3_nic_cmd {
	/* FUNC CFG */
	HINIC3_NIC_CMD_SET_FUNC_TBL              = 5,
	HINIC3_NIC_CMD_SET_VPORT_ENABLE          = 6,
	HINIC3_NIC_CMD_SQ_CI_ATTR_SET            = 8,
	HINIC3_NIC_CMD_CLEAR_QP_RESOURCE         = 11,
	HINIC3_NIC_CMD_FEATURE_NEGO              = 15,
	HINIC3_NIC_CMD_SET_MAC                   = 21,
	HINIC3_NIC_CMD_DEL_MAC                   = 22,
	HINIC3_NIC_CMD_UPDATE_MAC                = 23,
	HINIC3_NIC_CMD_RSS_CFG                   = 60,
	HINIC3_NIC_CMD_CFG_RSS_HASH_KEY          = 63,
	HINIC3_NIC_CMD_CFG_RSS_HASH_ENGINE       = 64,
	HINIC3_NIC_CMD_SET_RSS_CTX_TBL_INTO_FUNC = 65,
	HINIC3_NIC_CMD_QOS_DCB_STATE             = 110,
	HINIC3_NIC_CMD_FORCE_PKT_DROP            = 113,
};

#endif
