// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/if_vlan.h>

#include "hinic3_nic_cfg.h"
#include "hinic3_nic_io.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
#include "hinic3_nic_dev.h"
#include "hinic3_hwif.h"

int l2nic_msg_to_mgmt_sync(struct hinic3_hwdev *hwdev, u16 cmd,
			   const void *buf_in, u32 in_size,
			   void *buf_out, u32 *out_size)
{
	return hinic3_send_mbox_to_mgmt(hwdev, HINIC3_MOD_L2NIC, cmd, buf_in,
					in_size, buf_out, out_size, 0);
}

static int nic_feature_nego(struct hinic3_hwdev *hwdev, u8 opcode, u64 *s_feature, u16 size)
{
	struct hinic3_cmd_feature_nego feature_nego;
	u32 out_size = sizeof(feature_nego);
	int err;

	memset(&feature_nego, 0, sizeof(feature_nego));
	feature_nego.func_id = hinic3_global_func_id(hwdev);
	feature_nego.opcode = opcode;
	if (opcode == HINIC3_CMD_OP_SET)
		memcpy(feature_nego.s_feature, s_feature, size * sizeof(u64));

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_FEATURE_NEGO,
				     &feature_nego, sizeof(feature_nego),
				     &feature_nego, &out_size);
	if (err || !out_size || feature_nego.msg_head.status) {
		dev_err(hwdev->dev, "Failed to negotiate nic feature, err:%d, status: 0x%x, out_size: 0x%x\n",
			err, feature_nego.msg_head.status, out_size);
		return -EIO;
	}

	if (opcode == HINIC3_CMD_OP_GET)
		memcpy(s_feature, feature_nego.s_feature, size * sizeof(u64));

	return 0;
}

int hinic3_set_nic_feature_to_hw(struct hinic3_nic_dev *nic_dev)
{
	return nic_feature_nego(nic_dev->hwdev, HINIC3_CMD_OP_SET,
				&nic_dev->nic_io->feature_cap, 1);
}

bool hinic3_test_support(struct hinic3_nic_dev *nic_dev,
			 enum nic_feature_cap feature_bits)
{
	return (nic_dev->nic_io->feature_cap & feature_bits) == feature_bits;
}

void hinic3_update_nic_feature(struct hinic3_nic_dev *nic_dev, u64 feature_cap)
{
	nic_dev->nic_io->feature_cap = feature_cap;
}

static int hinic3_set_function_table(struct hinic3_hwdev *hwdev, u32 cfg_bitmap,
				     const struct hinic3_func_tbl_cfg *cfg)
{
	struct hinic3_cmd_set_func_tbl cmd_func_tbl;
	u32 out_size = sizeof(cmd_func_tbl);
	int err;

	memset(&cmd_func_tbl, 0, sizeof(cmd_func_tbl));
	cmd_func_tbl.func_id = hinic3_global_func_id(hwdev);
	cmd_func_tbl.cfg_bitmap = cfg_bitmap;
	cmd_func_tbl.tbl_cfg = *cfg;

	err = l2nic_msg_to_mgmt_sync(hwdev,
				     HINIC3_NIC_CMD_SET_FUNC_TBL,
				     &cmd_func_tbl, sizeof(cmd_func_tbl),
				     &cmd_func_tbl, &out_size);
	if (err || cmd_func_tbl.msg_head.status || !out_size) {
		dev_err(hwdev->dev,
			"Failed to set func table, bitmap: 0x%x, err: %d, status: 0x%x, out size: 0x%x\n",
			cfg_bitmap, err, cmd_func_tbl.msg_head.status,
			out_size);
		return -EFAULT;
	}

	return 0;
}

int hinic3_set_port_mtu(struct net_device *netdev, u16 new_mtu)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_func_tbl_cfg func_tbl_cfg = {};
	struct hinic3_hwdev *hwdev = nic_dev->hwdev;

	if (new_mtu < HINIC3_MIN_MTU_SIZE) {
		dev_err(hwdev->dev,
			"Invalid mtu size: %ubytes, mtu size < %ubytes\n",
			new_mtu, HINIC3_MIN_MTU_SIZE);
		return -EINVAL;
	}

	if (new_mtu > HINIC3_MAX_JUMBO_FRAME_SIZE) {
		dev_err(hwdev->dev, "Invalid mtu size: %ubytes, mtu size > %ubytes\n",
			new_mtu, HINIC3_MAX_JUMBO_FRAME_SIZE);
		return -EINVAL;
	}

	func_tbl_cfg.mtu = new_mtu;
	return hinic3_set_function_table(hwdev, BIT(FUNC_CFG_MTU),
					 &func_tbl_cfg);
}

static int hinic3_check_mac_info(struct hinic3_hwdev *hwdev, u8 status, u16 vlan_id)
{
	if ((status && status != HINIC3_MGMT_STATUS_EXIST) ||
	    ((vlan_id & CHECK_IPSU_15BIT) &&
	     status == HINIC3_MGMT_STATUS_EXIST)) {
		return -EINVAL;
	}

	return 0;
}

#define HINIC_VLAN_ID_MASK  0x7FFF

int hinic3_set_mac(struct hinic3_hwdev *hwdev, const u8 *mac_addr, u16 vlan_id, u16 func_id)
{
	struct hinic3_port_mac_set mac_info;
	u32 out_size = sizeof(mac_info);
	int err;

	if ((vlan_id & HINIC_VLAN_ID_MASK) >= VLAN_N_VID) {
		dev_err(hwdev->dev, "Invalid VLAN number: %d\n",
			(vlan_id & HINIC_VLAN_ID_MASK));
		return -EINVAL;
	}

	memset(&mac_info, 0, sizeof(mac_info));
	mac_info.func_id = func_id;
	mac_info.vlan_id = vlan_id;
	ether_addr_copy(mac_info.mac, mac_addr);

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_SET_MAC,
				     &mac_info, sizeof(mac_info),
				     &mac_info, &out_size);
	if (err || !out_size ||
	    hinic3_check_mac_info(hwdev, mac_info.msg_head.status,
				  mac_info.vlan_id)) {
		dev_err(hwdev->dev,
			"Failed to update MAC, err: %d, status: 0x%x, out size: 0x%x\n",
			err, mac_info.msg_head.status, out_size);
		return -EIO;
	}

	if (mac_info.msg_head.status == HINIC3_PF_SET_VF_ALREADY) {
		dev_warn(hwdev->dev, "PF has already set VF mac, Ignore set operation\n");
		return 0;
	}

	if (mac_info.msg_head.status == HINIC3_MGMT_STATUS_EXIST) {
		dev_warn(hwdev->dev, "MAC is repeated. Ignore update operation\n");
		return 0;
	}

	return 0;
}

int hinic3_del_mac(struct hinic3_hwdev *hwdev, const u8 *mac_addr, u16 vlan_id, u16 func_id)
{
	struct hinic3_port_mac_set mac_info;
	u32 out_size = sizeof(mac_info);
	int err;

	if ((vlan_id & HINIC_VLAN_ID_MASK) >= VLAN_N_VID) {
		dev_err(hwdev->dev, "Invalid VLAN number: %d\n",
			(vlan_id & HINIC_VLAN_ID_MASK));
		return -EINVAL;
	}

	memset(&mac_info, 0, sizeof(mac_info));
	mac_info.func_id = func_id;
	mac_info.vlan_id = vlan_id;
	ether_addr_copy(mac_info.mac, mac_addr);

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_DEL_MAC,
				     &mac_info, sizeof(mac_info), &mac_info,
				     &out_size);
	if (err || !out_size) {
		dev_err(hwdev->dev,
			"Failed to delete MAC, err: %d, status: 0x%x, out size: 0x%x\n",
			err, mac_info.msg_head.status, out_size);
		return -EIO;
	}

	return 0;
}

int hinic3_update_mac(struct hinic3_hwdev *hwdev, const u8 *old_mac, u8 *new_mac,
		      u16 vlan_id, u16 func_id)
{
	struct hinic3_port_mac_update mac_info;
	u32 out_size = sizeof(mac_info);
	int err;

	if ((vlan_id & HINIC_VLAN_ID_MASK) >= VLAN_N_VID) {
		dev_err(hwdev->dev, "Invalid VLAN number: %d\n",
			(vlan_id & HINIC_VLAN_ID_MASK));
		return -EINVAL;
	}

	memset(&mac_info, 0, sizeof(mac_info));
	mac_info.func_id = func_id;
	mac_info.vlan_id = vlan_id;
	ether_addr_copy(mac_info.old_mac, old_mac);
	ether_addr_copy(mac_info.new_mac, new_mac);

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_UPDATE_MAC,
				     &mac_info, sizeof(mac_info),
				     &mac_info, &out_size);
	if (err || !out_size ||
	    hinic3_check_mac_info(hwdev, mac_info.msg_head.status,
				  mac_info.vlan_id)) {
		dev_err(hwdev->dev,
			"Failed to update MAC, err: %d, status: 0x%x, out size: 0x%x\n",
			err, mac_info.msg_head.status, out_size);
		return -EIO;
	}
	return 0;
}

int hinic3_force_drop_tx_pkt(struct hinic3_hwdev *hwdev)
{
	struct hinic3_force_pkt_drop pkt_drop;
	u32 out_size = sizeof(pkt_drop);
	int err;

	memset(&pkt_drop, 0, sizeof(pkt_drop));
	pkt_drop.port = hinic3_physical_port_id(hwdev);
	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_FORCE_PKT_DROP,
				     &pkt_drop, sizeof(pkt_drop),
				     &pkt_drop, &out_size);
	if ((pkt_drop.msg_head.status != MGMT_CMD_UNSUPPORTED &&
	     pkt_drop.msg_head.status) || err || !out_size) {
		dev_err(hwdev->dev,
			"Failed to set force tx packets drop, err: %d, status: 0x%x, out size: 0x%x\n",
			err, pkt_drop.msg_head.status, out_size);
		return -EFAULT;
	}

	return pkt_drop.msg_head.status;
}
