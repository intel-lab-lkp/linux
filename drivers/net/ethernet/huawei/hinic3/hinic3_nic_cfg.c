// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/netdevice.h>

#include "hinic3_hw_comm.h"
#include "hinic3_hw_cfg.h"
#include "hinic3_mgmt.h"
#include "hinic3_nic_dev.h"
#include "hinic3_mgmt_interface.h"
#include "hinic3_nic_io.h"
#include "hinic3_nic_cfg.h"

#define HINIC3_CMD_OP_ADD  1
#define HINIC3_CMD_OP_DEL  0

int l2nic_msg_to_mgmt_sync(struct hinic3_hwdev *hwdev, u16 cmd,
			   const void *buf_in, u32 in_size,
			   void *buf_out, u32 *out_size)
{
	return hinic3_msg_to_mgmt_sync(hwdev, HINIC3_MOD_L2NIC, cmd, buf_in,
				       in_size, buf_out, out_size, 0);
}

static int nic_feature_nego(struct hinic3_hwdev *hwdev, u8 opcode, u64 *s_feature, u16 size)
{
	struct hinic3_cmd_feature_nego feature_nego;
	u32 out_size = sizeof(feature_nego);
	int err;

	if (size > NIC_MAX_FEATURE_QWORD)
		return -EINVAL;

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

int hinic3_get_nic_feature_from_hw(struct hinic3_nic_dev *nic_dev)
{
	return nic_feature_nego(nic_dev->hwdev, HINIC3_CMD_OP_GET,
				&nic_dev->nic_io->feature_cap, 1);
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

static int hinic3_set_rx_lro(struct hinic3_hwdev *hwdev, u8 ipv4_en, u8 ipv6_en,
			     u8 lro_max_pkt_len)
{
	struct hinic3_cmd_lro_config lro_cfg;
	u32 out_size = sizeof(lro_cfg);
	int err;

	memset(&lro_cfg, 0, sizeof(lro_cfg));
	lro_cfg.func_id = hinic3_global_func_id(hwdev);
	lro_cfg.opcode = HINIC3_CMD_OP_SET;
	lro_cfg.lro_ipv4_en = ipv4_en;
	lro_cfg.lro_ipv6_en = ipv6_en;
	lro_cfg.lro_max_pkt_len = lro_max_pkt_len;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_CFG_RX_LRO,
				     &lro_cfg, sizeof(lro_cfg),
				     &lro_cfg, &out_size);
	if (err || !out_size || lro_cfg.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set lro offload, err: %d, status: 0x%x, out size: 0x%x\n",
			err, lro_cfg.msg_head.status, out_size);
		return -EINVAL;
	}

	return 0;
}

static int hinic3_set_rx_lro_timer(struct hinic3_hwdev *hwdev, u32 timer_value)
{
	struct hinic3_cmd_lro_timer lro_timer;
	u32 out_size = sizeof(lro_timer);
	int err;

	memset(&lro_timer, 0, sizeof(lro_timer));
	lro_timer.opcode = HINIC3_CMD_OP_SET;
	lro_timer.timer = timer_value;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_CFG_LRO_TIMER,
				     &lro_timer, sizeof(lro_timer),
				     &lro_timer, &out_size);
	if (err || !out_size || lro_timer.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set lro timer, err: %d, status: 0x%x, out size: 0x%x\n",
			err, lro_timer.msg_head.status, out_size);

		return -EINVAL;
	}

	return 0;
}

int hinic3_set_rx_lro_state(struct hinic3_hwdev *hwdev, u8 lro_en, u32 lro_timer,
			    u8 lro_max_pkt_len)
{
	u8 ipv4_en, ipv6_en;
	int err;

	ipv4_en = lro_en ? 1 : 0;
	ipv6_en = lro_en ? 1 : 0;

	dev_dbg(hwdev->dev, "Set LRO max coalesce packet size to %uK\n", lro_max_pkt_len);

	err = hinic3_set_rx_lro(hwdev, ipv4_en, ipv6_en, lro_max_pkt_len);
	if (err)
		return err;

	/* we don't set LRO timer for VF */
	if (HINIC3_IS_VF(hwdev))
		return 0;

	dev_dbg(hwdev->dev, "Set LRO timer to %u\n", lro_timer);

	return hinic3_set_rx_lro_timer(hwdev, lro_timer);
}

int hinic3_set_rx_vlan_offload(struct hinic3_hwdev *hwdev, u8 en)
{
	struct hinic3_cmd_vlan_offload vlan_cfg;
	u32 out_size = sizeof(vlan_cfg);
	int err;

	memset(&vlan_cfg, 0, sizeof(vlan_cfg));
	vlan_cfg.func_id = hinic3_global_func_id(hwdev);
	vlan_cfg.vlan_offload = en;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_SET_RX_VLAN_OFFLOAD,
				     &vlan_cfg, sizeof(vlan_cfg),
				     &vlan_cfg, &out_size);
	if (err || !out_size || vlan_cfg.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set rx vlan offload, err: %d, status: 0x%x, out size: 0x%x\n",
			err, vlan_cfg.msg_head.status, out_size);
		return -EINVAL;
	}

	return 0;
}

int hinic3_set_vlan_fliter(struct hinic3_hwdev *hwdev, u32 vlan_filter_ctrl)
{
	struct hinic3_cmd_set_vlan_filter vlan_filter;
	u32 out_size = sizeof(vlan_filter);
	int err;

	memset(&vlan_filter, 0, sizeof(vlan_filter));
	vlan_filter.func_id = hinic3_global_func_id(hwdev);
	vlan_filter.vlan_filter_ctrl = vlan_filter_ctrl;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_SET_VLAN_FILTER_EN,
				     &vlan_filter, sizeof(vlan_filter),
				     &vlan_filter, &out_size);
	if (err || !out_size || vlan_filter.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set vlan filter, err: %d, status: 0x%x, out size: 0x%x\n",
			err, vlan_filter.msg_head.status, out_size);
		return -EINVAL;
	}

	return 0;
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

int hinic3_init_function_table(struct hinic3_nic_dev *nic_dev)
{
	struct hinic3_nic_io *nic_io = nic_dev->nic_io;
	struct hinic3_func_tbl_cfg func_tbl_cfg;
	u32 cfg_bitmap;

	func_tbl_cfg.mtu = 0x3FFF; /* default, max mtu */
	func_tbl_cfg.rx_wqe_buf_size = nic_io->rx_buff_len;

	cfg_bitmap = BIT(FUNC_CFG_INIT) | BIT(FUNC_CFG_MTU) | BIT(FUNC_CFG_RX_BUF_SIZE);
	return hinic3_set_function_table(nic_dev->hwdev, cfg_bitmap, &func_tbl_cfg);
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

#define PF_SET_VF_MAC(hwdev, status) \
	(HINIC3_IS_VF(hwdev) && (status) == HINIC3_PF_SET_VF_ALREADY)

static int hinic3_check_mac_info(struct hinic3_hwdev *hwdev, u8 status, u16 vlan_id)
{
	if ((status && status != HINIC3_MGMT_STATUS_EXIST) ||
	    ((vlan_id & CHECK_IPSU_15BIT) &&
	     status == HINIC3_MGMT_STATUS_EXIST)) {
		if (PF_SET_VF_MAC(hwdev, status))
			return 0;

		return -EINVAL;
	}

	return 0;
}

int hinic3_get_default_mac(struct hinic3_hwdev *hwdev, u8 *mac_addr)
{
	struct hinic3_port_mac_set mac_info;
	u32 out_size = sizeof(mac_info);
	int err;

	memset(&mac_info, 0, sizeof(mac_info));
	mac_info.func_id = hinic3_global_func_id(hwdev);

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_GET_MAC,
				     &mac_info, sizeof(mac_info),
				     &mac_info, &out_size);
	if (err || !out_size || mac_info.msg_head.status) {
		dev_err(hwdev->dev,
			"Failed to get mac, err: %d, status: 0x%x, out size: 0x%x\n",
			err, mac_info.msg_head.status, out_size);
		return -EINVAL;
	}

	ether_addr_copy(mac_addr, mac_info.mac);

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

	if (PF_SET_VF_MAC(hwdev, mac_info.msg_head.status)) {
		dev_warn(hwdev->dev, "PF has already set VF mac, Ignore set operation\n");
		return HINIC3_PF_SET_VF_ALREADY;
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
	if (err || !out_size ||
	    (mac_info.msg_head.status && !PF_SET_VF_MAC(hwdev, mac_info.msg_head.status))) {
		dev_err(hwdev->dev,
			"Failed to delete MAC, err: %d, status: 0x%x, out size: 0x%x\n",
			err, mac_info.msg_head.status, out_size);
		return -EIO;
	}

	if (PF_SET_VF_MAC(hwdev, mac_info.msg_head.status)) {
		dev_warn(hwdev->dev, "PF has already set VF mac, Ignore delete operation.\n");
		return HINIC3_PF_SET_VF_ALREADY;
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

	if (PF_SET_VF_MAC(hwdev, mac_info.msg_head.status)) {
		dev_warn(hwdev->dev, "PF has already set VF MAC. Ignore update operation\n");
		return HINIC3_PF_SET_VF_ALREADY;
	}

	if (mac_info.msg_head.status == HINIC3_MGMT_STATUS_EXIST) {
		dev_warn(hwdev->dev, "MAC is repeated. Ignore update operation\n");
		return 0;
	}

	return 0;
}

int hinic3_set_ci_table(struct hinic3_hwdev *hwdev, struct hinic3_sq_attr *attr)
{
	struct hinic3_cmd_cons_idx_attr cons_idx_attr;
	u32 out_size = sizeof(cons_idx_attr);
	int err;

	memset(&cons_idx_attr, 0, sizeof(cons_idx_attr));
	cons_idx_attr.func_idx = hinic3_global_func_id(hwdev);
	cons_idx_attr.dma_attr_off  = attr->dma_attr_off;
	cons_idx_attr.pending_limit = attr->pending_limit;
	cons_idx_attr.coalescing_time  = attr->coalescing_time;

	if (attr->intr_en) {
		cons_idx_attr.intr_en = attr->intr_en;
		cons_idx_attr.intr_idx = attr->intr_idx;
	}

	cons_idx_attr.l2nic_sqn = attr->l2nic_sqn;
	cons_idx_attr.ci_addr = attr->ci_dma_base;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_SQ_CI_ATTR_SET,
				     &cons_idx_attr, sizeof(cons_idx_attr),
				     &cons_idx_attr, &out_size);
	if (err || !out_size || cons_idx_attr.msg_head.status) {
		dev_err(hwdev->dev,
			"Failed to set ci attribute table, err: %d, status: 0x%x, out_size: 0x%x\n",
			err, cons_idx_attr.msg_head.status, out_size);
		return -EFAULT;
	}

	return 0;
}

int hinic3_flush_qps_res(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmd_clear_qp_resource sq_res;
	u32 out_size = sizeof(sq_res);
	int err;

	memset(&sq_res, 0, sizeof(sq_res));
	sq_res.func_id = hinic3_global_func_id(hwdev);

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_CLEAR_QP_RESOURCE,
				     &sq_res, sizeof(sq_res), &sq_res,
				     &out_size);
	if (err || !out_size || sq_res.msg_head.status) {
		dev_err(hwdev->dev, "Failed to clear sq resources, err: %d, status: 0x%x, out size: 0x%x\n",
			err, sq_res.msg_head.status, out_size);
		return -EINVAL;
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

int hinic3_set_rx_mode(struct hinic3_hwdev *hwdev, u32 enable)
{
	struct hinic3_rx_mode_config rx_mode_cfg;
	u32 out_size = sizeof(rx_mode_cfg);
	int err;

	memset(&rx_mode_cfg, 0, sizeof(rx_mode_cfg));
	rx_mode_cfg.func_id = hinic3_global_func_id(hwdev);
	rx_mode_cfg.rx_mode = enable;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_SET_RX_MODE,
				     &rx_mode_cfg, sizeof(rx_mode_cfg),
				     &rx_mode_cfg, &out_size);
	if (err || !out_size || rx_mode_cfg.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set rx mode, err: %d, status: 0x%x, out size: 0x%x\n",
			err, rx_mode_cfg.msg_head.status, out_size);
		return -EINVAL;
	}

	return 0;
}

static int hinic3_config_vlan(struct hinic3_hwdev *hwdev, u8 opcode,
			      u16 vlan_id, u16 func_id)
{
	struct hinic3_cmd_vlan_config vlan_info;
	u32 out_size = sizeof(vlan_info);
	int err;

	memset(&vlan_info, 0, sizeof(vlan_info));
	vlan_info.opcode = opcode;
	vlan_info.func_id = func_id;
	vlan_info.vlan_id = vlan_id;

	err = l2nic_msg_to_mgmt_sync(hwdev,
				     HINIC3_NIC_CMD_CFG_FUNC_VLAN,
				     &vlan_info, sizeof(vlan_info),
				     &vlan_info, &out_size);
	if (err || !out_size || vlan_info.msg_head.status) {
		dev_err(hwdev->dev,
			"Failed to %s vlan, err: %d, status: 0x%x, out size: 0x%x\n",
			opcode == HINIC3_CMD_OP_ADD ? "add" : "delete",
			err, vlan_info.msg_head.status, out_size);
		return -EINVAL;
	}

	return 0;
}

int hinic3_add_vlan(struct hinic3_hwdev *hwdev, u16 vlan_id, u16 func_id)
{
	return hinic3_config_vlan(hwdev, HINIC3_CMD_OP_ADD, vlan_id, func_id);
}

int hinic3_del_vlan(struct hinic3_hwdev *hwdev, u16 vlan_id, u16 func_id)
{
	return hinic3_config_vlan(hwdev, HINIC3_CMD_OP_DEL, vlan_id, func_id);
}

int hinic3_sync_dcb_state(struct hinic3_hwdev *hwdev, u8 op_code, u8 state)
{
	struct hinic3_cmd_set_dcb_state dcb_state;
	u32 out_size = sizeof(dcb_state);
	int err;

	memset(&dcb_state, 0, sizeof(dcb_state));
	dcb_state.op_code = op_code;
	dcb_state.state = state;
	dcb_state.func_id = hinic3_global_func_id(hwdev);

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_QOS_DCB_STATE,
				     &dcb_state, sizeof(dcb_state), &dcb_state, &out_size);
	if (err || dcb_state.head.status || !out_size) {
		dev_err(hwdev->dev,
			"Failed to set dcb state, err: %d, status: 0x%x, out size: 0x%x\n",
			err, dcb_state.head.status, out_size);
		return -EFAULT;
	}

	return 0;
}

int hinic3_set_port_enable(struct hinic3_hwdev *hwdev, bool enable)
{
	struct mag_cmd_set_port_enable en_state;
	u32 out_size = sizeof(en_state);
	int err;

	if (HINIC3_IS_VF(hwdev))
		return 0;

	memset(&en_state, 0, sizeof(en_state));
	en_state.function_id = hinic3_global_func_id(hwdev);
	en_state.state = enable ? MAG_CMD_TX_ENABLE | MAG_CMD_RX_ENABLE :
				MAG_CMD_PORT_DISABLE;

	err = hinic3_msg_to_mgmt_sync(hwdev, HINIC3_MOD_HILINK, MAG_CMD_SET_PORT_ENABLE, &en_state,
				      sizeof(en_state), &en_state, &out_size, 0);
	if (err || !out_size || en_state.head.status) {
		dev_err(hwdev->dev, "Failed to set port state, err: %d, status: 0x%x, out size: 0x%x\n",
			err, en_state.head.status, out_size);
		return -EIO;
	}

	return 0;
}

int hinic3_get_link_status(struct hinic3_hwdev *hwdev, bool *link_status_up)
{
	struct mag_cmd_get_link_status get_link;
	u32 out_size = sizeof(get_link);
	int err;

	memset(&get_link, 0, sizeof(get_link));
	get_link.port_id = hinic3_physical_port_id(hwdev);

	err = hinic3_msg_to_mgmt_sync(hwdev, HINIC3_MOD_HILINK, MAG_CMD_GET_LINK_STATUS, &get_link,
				      sizeof(get_link), &get_link, &out_size, 0);
	if (err || !out_size || get_link.head.status) {
		dev_err(hwdev->dev, "Failed to get link state, err: %d, status: 0x%x, out size: 0x%x\n",
			err, get_link.head.status, out_size);
		return -EIO;
	}

	*link_status_up = !!get_link.status;

	return 0;
}

int hinic3_get_phy_port_stats(struct hinic3_hwdev *hwdev, struct mag_cmd_port_stats *stats)
{
	struct mag_cmd_port_stats_info stats_info;
	struct mag_cmd_get_port_stat *port_stats;
	u32 out_size = sizeof(*port_stats);
	int err;

	port_stats = kzalloc(sizeof(*port_stats), GFP_KERNEL);
	if (!port_stats)
		return -ENOMEM;

	memset(&stats_info, 0, sizeof(stats_info));
	stats_info.port_id = hinic3_physical_port_id(hwdev);

	err = hinic3_msg_to_mgmt_sync(hwdev, HINIC3_MOD_HILINK, MAG_CMD_GET_PORT_STAT,
				      &stats_info, sizeof(stats_info),
				      port_stats, &out_size, 0);
	if (err || !out_size || port_stats->head.status) {
		dev_err(hwdev->dev,
			"Failed to get port statistics, err: %d, status: 0x%x, out size: 0x%x\n",
			err, port_stats->head.status, out_size);
		err = -EIO;
		goto out;
	}

	memcpy(stats, &port_stats->counter, sizeof(*stats));

out:
	kfree(port_stats);

	return err;
}

int hinic3_get_port_info(struct hinic3_hwdev *hwdev, struct nic_port_info *port_info)
{
	struct mag_cmd_get_port_info port_msg;
	u32 out_size = sizeof(port_msg);
	int err;

	memset(&port_msg, 0, sizeof(port_msg));
	port_msg.port_id = hinic3_physical_port_id(hwdev);

	err = hinic3_msg_to_mgmt_sync(hwdev, HINIC3_MOD_HILINK, MAG_CMD_GET_PORT_INFO, &port_msg,
				      sizeof(port_msg), &port_msg, &out_size, 0);
	if (err || !out_size || port_msg.head.status) {
		dev_err(hwdev->dev,
			"Failed to get port info, err: %d, status: 0x%x, out size: 0x%x\n",
			err, port_msg.head.status, out_size);
		return -EIO;
	}

	port_info->autoneg_cap = port_msg.an_support;
	port_info->autoneg_state = port_msg.an_en;
	port_info->duplex = port_msg.duplex;
	port_info->port_type = port_msg.wire_type;
	port_info->speed = port_msg.speed;
	port_info->fec = port_msg.fec;
	port_info->supported_mode = port_msg.supported_mode;
	port_info->advertised_mode = port_msg.advertised_mode;

	return 0;
}

int hinic3_set_vport_enable(struct hinic3_hwdev *hwdev, u16 func_id, bool enable)
{
	struct hinic3_vport_state en_state;
	u32 out_size = sizeof(en_state);
	int err;

	memset(&en_state, 0, sizeof(en_state));
	en_state.func_id = func_id;
	en_state.state = enable ? 1 : 0;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_SET_VPORT_ENABLE,
				     &en_state, sizeof(en_state),
				     &en_state, &out_size);
	if (err || !out_size || en_state.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set vport state, err: %d, status: 0x%x, out size: 0x%x\n",
			err, en_state.msg_head.status, out_size);
		return -EINVAL;
	}

	return 0;
}

#define UNSUPPORT_SET_PAUSE     0x10
static int hinic3_cfg_hw_pause(struct hinic3_hwdev *hwdev, u8 opcode,
			       struct nic_pause_config *nic_pause)
{
	struct hinic3_cmd_pause_config pause_info;
	u32 out_size = sizeof(pause_info);
	int err;

	memset(&pause_info, 0, sizeof(pause_info));

	pause_info.port_id = hinic3_physical_port_id(hwdev);
	pause_info.opcode = opcode;
	if (opcode == HINIC3_CMD_OP_SET) {
		pause_info.auto_neg = nic_pause->auto_neg;
		pause_info.rx_pause = nic_pause->rx_pause;
		pause_info.tx_pause = nic_pause->tx_pause;
	}

	err = l2nic_msg_to_mgmt_sync(hwdev,
				     HINIC3_NIC_CMD_CFG_PAUSE_INFO,
				     &pause_info, sizeof(pause_info),
				     &pause_info, &out_size);
	if (err || !out_size || pause_info.msg_head.status) {
		if (pause_info.msg_head.status == UNSUPPORT_SET_PAUSE) {
			err = -EOPNOTSUPP;
			dev_err(hwdev->dev, "Can not set pause when pfc is enabled\n");
		} else {
			err = -EFAULT;
			dev_err(hwdev->dev, "Failed to %s pause info, err: %d, status: 0x%x, out size: 0x%x\n",
				opcode == HINIC3_CMD_OP_SET ? "set" : "get",
				err, pause_info.msg_head.status, out_size);
		}
		return err;
	}

	if (opcode == HINIC3_CMD_OP_GET) {
		nic_pause->auto_neg = pause_info.auto_neg;
		nic_pause->rx_pause = pause_info.rx_pause;
		nic_pause->tx_pause = pause_info.tx_pause;
	}

	return 0;
}

int hinic3_get_pause_info(struct hinic3_nic_dev *nic_dev,
			  struct nic_pause_config *nic_pause)
{
	struct hinic3_nic_io *nic_io = nic_dev->nic_io;
	struct hinic3_nic_cfg *nic_cfg;
	int err;

	err = hinic3_cfg_hw_pause(nic_dev->hwdev, HINIC3_CMD_OP_GET, nic_pause);
	if (err)
		return err;

	nic_cfg = &nic_io->nic_cfg;
	if (nic_cfg->pause_set || !nic_pause->auto_neg) {
		nic_pause->rx_pause = nic_cfg->nic_pause.rx_pause;
		nic_pause->tx_pause = nic_cfg->nic_pause.tx_pause;
	}

	return 0;
}

int hinic3_get_vport_stats(struct hinic3_hwdev *hwdev, u16 func_id,
			   struct hinic3_vport_stats *stats)
{
	struct hinic3_cmd_vport_stats vport_stats;
	struct hinic3_port_stats_info stats_info;
	u32 out_size = sizeof(vport_stats);
	int err;

	memset(&stats_info, 0, sizeof(stats_info));
	memset(&vport_stats, 0, sizeof(vport_stats));
	stats_info.func_id = func_id;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_GET_VPORT_STAT,
				     &stats_info, sizeof(stats_info),
				     &vport_stats, &out_size);
	if (err || !out_size || vport_stats.msg_head.status) {
		dev_err(hwdev->dev,
			"Failed to get function statistics, err: %d, status: 0x%x, out size: 0x%x\n",
			err, vport_stats.msg_head.status, out_size);
		return -EFAULT;
	}

	memcpy(stats, &vport_stats.stats, sizeof(*stats));

	return 0;
}
