/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_NIC_CFG_H
#define HINIC3_NIC_CFG_H

#include <linux/types.h>

#include "hinic3_mgmt_interface.h"
#include "hinic3_hw_intf.h"

struct hinic3_hwdev;
struct hinic3_nic_dev;

#define HINIC3_MIN_MTU_SIZE             256
#define HINIC3_MAX_JUMBO_FRAME_SIZE     9600

#define HINIC3_PF_SET_VF_ALREADY        0x4
#define HINIC3_MGMT_STATUS_EXIST        0x6
#define CHECK_IPSU_15BIT                0x8000

enum hinic3_nic_event_type {
	EVENT_NIC_LINK_DOWN = 0,
	EVENT_NIC_LINK_UP = 1,
};

int l2nic_msg_to_mgmt_sync(struct hinic3_hwdev *hwdev, u16 cmd,
			   const void *buf_in, u32 in_size,
			   void *buf_out, u32 *out_size);

int hinic3_set_nic_feature_to_hw(struct hinic3_nic_dev *nic_dev);
bool hinic3_test_support(struct hinic3_nic_dev *nic_dev,
			 enum nic_feature_cap feature_bits);
void hinic3_update_nic_feature(struct hinic3_nic_dev *nic_dev, u64 feature_cap);

int hinic3_set_port_mtu(struct net_device *netdev, u16 new_mtu);

int hinic3_set_mac(struct hinic3_hwdev *hwdev, const u8 *mac_addr, u16 vlan_id, u16 func_id);
int hinic3_del_mac(struct hinic3_hwdev *hwdev, const u8 *mac_addr, u16 vlan_id, u16 func_id);
int hinic3_update_mac(struct hinic3_hwdev *hwdev, const u8 *old_mac, u8 *new_mac, u16 vlan_id,
		      u16 func_id);

int hinic3_force_drop_tx_pkt(struct hinic3_hwdev *hwdev);

#endif
