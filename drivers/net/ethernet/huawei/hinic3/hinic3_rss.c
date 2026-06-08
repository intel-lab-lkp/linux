// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include <linux/ethtool.h>

#include "hinic3_cmdq.h"
#include "hinic3_hwdev.h"
#include "hinic3_hwif.h"
#include "hinic3_mbox.h"
#include "hinic3_nic_cfg.h"
#include "hinic3_nic_dev.h"
#include "hinic3_rss.h"

static void hinic3_fillout_indir_tbl(struct net_device *netdev, u16 *indir)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 i, num_qps;

	num_qps = nic_dev->q_params.num_qps;
	for (i = 0; i < L2NIC_RSS_INDIR_SIZE; i++)
		indir[i] = ethtool_rxfh_indir_default(i, num_qps);
}

static int hinic3_rss_cfg(struct hinic3_hwdev *hwdev, u8 rss_en, u16 num_qps)
{
	struct mgmt_msg_params msg_params = {};
	struct l2nic_cmd_cfg_rss rss_cfg = {};
	int err;

	rss_cfg.func_id = hinic3_global_func_id(hwdev);
	rss_cfg.rss_en = rss_en;
	rss_cfg.rq_priority_number = 0;
	rss_cfg.num_qps = num_qps;

	mgmt_msg_params_init_default(&msg_params, &rss_cfg, sizeof(rss_cfg));

	err = hinic3_send_mbox_to_mgmt(hwdev, MGMT_MOD_L2NIC,
				       L2NIC_CMD_CFG_RSS, &msg_params);
	if (err || rss_cfg.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set rss cfg, err: %d, status: 0x%x\n",
			err, rss_cfg.msg_head.status);
		return -EINVAL;
	}

	return 0;
}

static void hinic3_init_rss_parameters(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	nic_dev->rss_hash_type = HINIC3_RSS_HASH_ENGINE_TYPE_XOR;
	nic_dev->rss_type.tcp_ipv6_ext = 1;
	nic_dev->rss_type.ipv6_ext = 1;
	nic_dev->rss_type.tcp_ipv6 = 1;
	nic_dev->rss_type.ipv6 = 1;
	nic_dev->rss_type.tcp_ipv4 = 1;
	nic_dev->rss_type.ipv4 = 1;
	nic_dev->rss_type.udp_ipv6 = 1;
	nic_dev->rss_type.udp_ipv4 = 1;
}

static void decide_num_qps(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	unsigned int dev_cpus;

	dev_cpus = netif_get_num_default_rss_queues();
	nic_dev->q_params.num_qps = min(dev_cpus, nic_dev->max_qps);
}

static int alloc_rss_resource(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	nic_dev->rss_hkey = kmalloc_array(L2NIC_RSS_KEY_SIZE,
					  sizeof(nic_dev->rss_hkey[0]),
					  GFP_KERNEL);
	if (!nic_dev->rss_hkey)
		return -ENOMEM;

	netdev_rss_key_fill(nic_dev->rss_hkey, L2NIC_RSS_KEY_SIZE);

	nic_dev->rss_indir = kcalloc(L2NIC_RSS_INDIR_SIZE, sizeof(u16),
				     GFP_KERNEL);
	if (!nic_dev->rss_indir) {
		kfree(nic_dev->rss_hkey);
		nic_dev->rss_hkey = NULL;
		return -ENOMEM;
	}

	return 0;
}

static int hinic3_rss_set_indir_tbl(struct hinic3_hwdev *hwdev,
				    const u16 *indir_table)
{
	struct l2nic_cmd_rss_set_indir_tbl *indir_tbl;
	struct hinic3_cmd_buf *cmd_buf;
	__le64 out_param;
	int err;
	u32 i;

	cmd_buf = hinic3_alloc_cmd_buf(hwdev);
	if (!cmd_buf) {
		dev_err(hwdev->dev, "Failed to allocate cmd buf\n");
		return -ENOMEM;
	}

	cmd_buf->size = cpu_to_le16(sizeof(struct l2nic_cmd_rss_set_indir_tbl));
	indir_tbl = cmd_buf->buf;
	memset(indir_tbl, 0, sizeof(*indir_tbl));

	for (i = 0; i < L2NIC_RSS_INDIR_SIZE; i++)
		indir_tbl->entry[i] = cpu_to_le16(indir_table[i]);

	hinic3_cmdq_buf_swab32(indir_tbl, sizeof(*indir_tbl));

	err = hinic3_cmdq_direct_resp(hwdev, MGMT_MOD_L2NIC,
				      L2NIC_UCODE_CMD_SET_RSS_INDIR_TBL,
				      cmd_buf, &out_param);
	if (err || out_param) {
		dev_err(hwdev->dev, "Failed to set rss indir table\n");
		err = -EFAULT;
	}

	hinic3_free_cmd_buf(hwdev, cmd_buf);

	return err;
}

static int hinic3_set_rss_type(struct hinic3_hwdev *hwdev,
			       struct hinic3_rss_type rss_type)
{
	struct l2nic_cmd_rss_ctx_tbl ctx_tbl = {};
	struct mgmt_msg_params msg_params = {};
	u32 ctx;
	int err;

	ctx_tbl.func_id = hinic3_global_func_id(hwdev);
	ctx = L2NIC_RSS_TYPE_SET(1, VALID) |
	      L2NIC_RSS_TYPE_SET(rss_type.ipv4, IPV4) |
	      L2NIC_RSS_TYPE_SET(rss_type.ipv6, IPV6) |
	      L2NIC_RSS_TYPE_SET(rss_type.ipv6_ext, IPV6_EXT) |
	      L2NIC_RSS_TYPE_SET(rss_type.tcp_ipv4, TCP_IPV4) |
	      L2NIC_RSS_TYPE_SET(rss_type.tcp_ipv6, TCP_IPV6) |
	      L2NIC_RSS_TYPE_SET(rss_type.tcp_ipv6_ext, TCP_IPV6_EXT) |
	      L2NIC_RSS_TYPE_SET(rss_type.udp_ipv4, UDP_IPV4) |
	      L2NIC_RSS_TYPE_SET(rss_type.udp_ipv6, UDP_IPV6);
	ctx_tbl.context = ctx;

	mgmt_msg_params_init_default(&msg_params, &ctx_tbl, sizeof(ctx_tbl));

	err = hinic3_send_mbox_to_mgmt(hwdev, MGMT_MOD_L2NIC,
				       L2NIC_CMD_SET_RSS_CTX_TBL, &msg_params);

	if (ctx_tbl.msg_head.status == MGMT_STATUS_CMD_UNSUPPORTED) {
		return -EOPNOTSUPP;
	} else if (err || ctx_tbl.msg_head.status) {
		dev_err(hwdev->dev, "mgmt Failed to set rss context offload, err: %d, status: 0x%x\n",
			err, ctx_tbl.msg_head.status);
		return -EINVAL;
	}

	return 0;
}

static int hinic3_get_rss_type(struct hinic3_hwdev *hwdev,
			       struct hinic3_rss_type *rss_type)
{
	struct l2nic_cmd_rss_ctx_tbl ctx_tbl = {};
	struct mgmt_msg_params msg_params = {};
	int err;

	ctx_tbl.func_id = hinic3_global_func_id(hwdev);

	mgmt_msg_params_init_default(&msg_params, &ctx_tbl, sizeof(ctx_tbl));

	err = hinic3_send_mbox_to_mgmt(hwdev, MGMT_MOD_L2NIC,
				       L2NIC_CMD_GET_RSS_CTX_TBL,
				       &msg_params);
	if (ctx_tbl.msg_head.status == MGMT_STATUS_CMD_UNSUPPORTED) {
		return -EOPNOTSUPP;
	} else if (err || ctx_tbl.msg_head.status) {
		dev_err(hwdev->dev, "Failed to get hash type, err: %d, status: 0x%x\n",
			err, ctx_tbl.msg_head.status);
		return -EINVAL;
	}

	rss_type->ipv4         = L2NIC_RSS_TYPE_GET(ctx_tbl.context, IPV4);
	rss_type->ipv6         = L2NIC_RSS_TYPE_GET(ctx_tbl.context, IPV6);
	rss_type->ipv6_ext     = L2NIC_RSS_TYPE_GET(ctx_tbl.context, IPV6_EXT);
	rss_type->tcp_ipv4     = L2NIC_RSS_TYPE_GET(ctx_tbl.context, TCP_IPV4);
	rss_type->tcp_ipv6     = L2NIC_RSS_TYPE_GET(ctx_tbl.context, TCP_IPV6);
	rss_type->tcp_ipv6_ext = L2NIC_RSS_TYPE_GET(ctx_tbl.context,
						    TCP_IPV6_EXT);
	rss_type->udp_ipv4     = L2NIC_RSS_TYPE_GET(ctx_tbl.context, UDP_IPV4);
	rss_type->udp_ipv6     = L2NIC_RSS_TYPE_GET(ctx_tbl.context, UDP_IPV6);

	return 0;
}

static int hinic3_rss_cfg_hash_type(struct hinic3_hwdev *hwdev, u8 opcode,
				    enum hinic3_rss_hash_type *type)
{
	struct l2nic_cmd_cfg_rss_engine hash_type_cmd = {};
	struct mgmt_msg_params msg_params = {};
	int err;

	hash_type_cmd.func_id = hinic3_global_func_id(hwdev);
	hash_type_cmd.opcode = opcode;

	if (opcode == MGMT_MSG_CMD_OP_SET)
		hash_type_cmd.hash_engine = *type;

	mgmt_msg_params_init_default(&msg_params, &hash_type_cmd,
				     sizeof(hash_type_cmd));

	err = hinic3_send_mbox_to_mgmt(hwdev, MGMT_MOD_L2NIC,
				       L2NIC_CMD_CFG_RSS_HASH_ENGINE,
				       &msg_params);
	if (err || hash_type_cmd.msg_head.status) {
		dev_err(hwdev->dev, "Failed to %s hash engine, err: %d, status: 0x%x\n",
			opcode == MGMT_MSG_CMD_OP_SET ? "set" : "get",
			err, hash_type_cmd.msg_head.status);
		return -EIO;
	}

	if (opcode == MGMT_MSG_CMD_OP_GET)
		*type = hash_type_cmd.hash_engine;

	return 0;
}

static int hinic3_rss_set_hash_type(struct hinic3_hwdev *hwdev,
				    enum hinic3_rss_hash_type type)
{
	return hinic3_rss_cfg_hash_type(hwdev, MGMT_MSG_CMD_OP_SET, &type);
}

static int hinic3_config_rss_hw_resource(struct net_device *netdev,
					 u16 *indir_tbl)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	err = hinic3_rss_set_indir_tbl(nic_dev->hwdev, indir_tbl);
	if (err)
		return err;

	err = hinic3_set_rss_type(nic_dev->hwdev, nic_dev->rss_type);
	if (err)
		return err;

	return hinic3_rss_set_hash_type(nic_dev->hwdev, nic_dev->rss_hash_type);
}

static int hinic3_rss_cfg_hash_key(struct hinic3_hwdev *hwdev, u8 opcode,
				   u8 *key)
{
	struct l2nic_cmd_cfg_rss_hash_key hash_key = {};
	struct mgmt_msg_params msg_params = {};
	int err;

	hash_key.func_id = hinic3_global_func_id(hwdev);
	hash_key.opcode = opcode;

	if (opcode == MGMT_MSG_CMD_OP_SET)
		memcpy(hash_key.key, key, L2NIC_RSS_KEY_SIZE);

	mgmt_msg_params_init_default(&msg_params, &hash_key, sizeof(hash_key));

	err = hinic3_send_mbox_to_mgmt(hwdev, MGMT_MOD_L2NIC,
				       L2NIC_CMD_CFG_RSS_HASH_KEY, &msg_params);
	if (err || hash_key.msg_head.status) {
		dev_err(hwdev->dev, "Failed to %s hash key, err: %d, status: 0x%x\n",
			opcode == MGMT_MSG_CMD_OP_SET ? "set" : "get",
			err, hash_key.msg_head.status);
		return -EINVAL;
	}

	if (opcode == MGMT_MSG_CMD_OP_GET)
		memcpy(key, hash_key.key, L2NIC_RSS_KEY_SIZE);

	return 0;
}

static int hinic3_rss_set_hash_key(struct hinic3_hwdev *hwdev, u8 *key)
{
	return hinic3_rss_cfg_hash_key(hwdev, MGMT_MSG_CMD_OP_SET, key);
}

static int hinic3_set_hw_rss_parameters(struct net_device *netdev, u8 rss_en)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	err = hinic3_rss_set_hash_key(nic_dev->hwdev, nic_dev->rss_hkey);
	if (err)
		return err;

	if (!netif_is_rxfh_configured(netdev))
		hinic3_fillout_indir_tbl(netdev, nic_dev->rss_indir);

	err = hinic3_config_rss_hw_resource(netdev, nic_dev->rss_indir);
	if (err)
		return err;

	err = hinic3_rss_cfg(nic_dev->hwdev, rss_en, nic_dev->q_params.num_qps);
	if (err)
		return err;

	return 0;
}

int hinic3_rss_init(struct net_device *netdev)
{
	return hinic3_set_hw_rss_parameters(netdev, 1);
}

void hinic3_rss_uninit(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	hinic3_rss_cfg(nic_dev->hwdev, 0, 0);
}

void hinic3_clear_rss_config(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	kfree(nic_dev->rss_hkey);
	nic_dev->rss_hkey = NULL;

	kfree(nic_dev->rss_indir);
	nic_dev->rss_indir = NULL;
}

void hinic3_try_to_enable_rss(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_hwdev *hwdev = nic_dev->hwdev;
	int err;

	nic_dev->max_qps = hinic3_func_max_qnum(hwdev);
	if (nic_dev->max_qps <= 1 ||
	    !hinic3_test_support(nic_dev, HINIC3_NIC_F_RSS))
		goto err_reset_q_params;

	err = alloc_rss_resource(netdev);
	if (err) {
		nic_dev->max_qps = 1;
		goto err_reset_q_params;
	}

	set_bit(HINIC3_RSS_ENABLE, &nic_dev->flags);
	decide_num_qps(netdev);
	hinic3_init_rss_parameters(netdev);
	err = hinic3_set_hw_rss_parameters(netdev, 0);
	if (err) {
		dev_err(hwdev->dev, "Failed to set hardware rss parameters\n");
		hinic3_clear_rss_config(netdev);
		nic_dev->max_qps = 1;
		goto err_reset_q_params;
	}

	return;

err_reset_q_params:
	clear_bit(HINIC3_RSS_ENABLE, &nic_dev->flags);
	nic_dev->q_params.num_qps = nic_dev->max_qps;
}

static int hinic3_set_l4_rss_hash_ops(const struct ethtool_rxnfc *cmd,
				      struct hinic3_rss_type *rss_type)
{
	u8 rss_l4_en;

	switch (cmd->data & (RXH_L4_B_0_1 | RXH_L4_B_2_3)) {
	case 0:
		rss_l4_en = 0;
		break;
	case (RXH_L4_B_0_1 | RXH_L4_B_2_3):
		rss_l4_en = 1;
		break;
	default:
		return -EINVAL;
	}

	switch (cmd->flow_type) {
	case TCP_V4_FLOW:
		rss_type->tcp_ipv4 = rss_l4_en;
		break;
	case TCP_V6_FLOW:
		rss_type->tcp_ipv6 = rss_l4_en;
		break;
	case UDP_V4_FLOW:
		rss_type->udp_ipv4 = rss_l4_en;
		break;
	case UDP_V6_FLOW:
		rss_type->udp_ipv6 = rss_l4_en;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int hinic3_update_rss_hash_opts(struct net_device *netdev,
				       struct ethtool_rxnfc *cmd,
				       struct hinic3_rss_type *rss_type)
{
	int err;

	switch (cmd->flow_type) {
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
	case UDP_V4_FLOW:
	case UDP_V6_FLOW:
		err = hinic3_set_l4_rss_hash_ops(cmd, rss_type);
		if (err)
			return err;

		break;
	case IPV4_FLOW:
		rss_type->ipv4 = 1;
		break;
	case IPV6_FLOW:
		rss_type->ipv6 = 1;
		break;
	default:
		netdev_err(netdev, "Unsupported flow type\n");
		return -EINVAL;
	}

	return 0;
}

static int hinic3_set_rss_hash_opts(struct net_device *netdev,
				    struct ethtool_rxnfc *cmd)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_rss_type rss_type;
	int err;

	if (!test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags)) {
		cmd->data = 0;
		netdev_err(netdev, "RSS is disable, not support to set flow-hash\n");
		return -EOPNOTSUPP;
	}

	/* RSS only supports hashing of IP addresses and L4 ports */
	if (cmd->data & ~(RXH_IP_SRC | RXH_IP_DST |
			  RXH_L4_B_0_1 | RXH_L4_B_2_3))
		return -EINVAL;

	/* Both IP addresses must be part of the hash tuple */
	if (!(cmd->data & RXH_IP_SRC) || !(cmd->data & RXH_IP_DST))
		return -EINVAL;

	/* L4 hash bits are not valid for pure L3 flow types */
	if ((cmd->flow_type == IPV4_FLOW || cmd->flow_type == IPV6_FLOW) &&
	    (cmd->data & (RXH_L4_B_0_1 | RXH_L4_B_2_3)))
		return -EINVAL;

	err = hinic3_get_rss_type(nic_dev->hwdev, &rss_type);
	if (err) {
		netdev_err(netdev, "Failed to get rss type\n");
		return err;
	}

	err = hinic3_update_rss_hash_opts(netdev, cmd, &rss_type);
	if (err)
		return err;

	err = hinic3_set_rss_type(nic_dev->hwdev, rss_type);
	if (err) {
		netdev_err(netdev, "Failed to set rss type\n");
		return err;
	}

	nic_dev->rss_type = rss_type;

	return 0;
}

static void convert_rss_l3_type(u8 rss_opt, struct ethtool_rxnfc *cmd)
{
	if (!rss_opt)
		cmd->data &= ~(RXH_IP_SRC | RXH_IP_DST);
}

static void convert_rss_l4_type(u8 rss_opt, struct ethtool_rxnfc *cmd)
{
	if (rss_opt)
		cmd->data |= RXH_L4_B_0_1 | RXH_L4_B_2_3;
}

static int hinic3_convert_rss_type(struct net_device *netdev,
				   struct hinic3_rss_type *rss_type,
				   struct ethtool_rxnfc *cmd)
{
	cmd->data = RXH_IP_SRC | RXH_IP_DST;
	switch (cmd->flow_type) {
	case TCP_V4_FLOW:
		convert_rss_l4_type(rss_type->tcp_ipv4, cmd);
		break;
	case TCP_V6_FLOW:
		convert_rss_l4_type(rss_type->tcp_ipv6, cmd);
		break;
	case UDP_V4_FLOW:
		convert_rss_l4_type(rss_type->udp_ipv4, cmd);
		break;
	case UDP_V6_FLOW:
		convert_rss_l4_type(rss_type->udp_ipv6, cmd);
		break;
	case IPV4_FLOW:
		convert_rss_l3_type(rss_type->ipv4, cmd);
		break;
	case IPV6_FLOW:
		convert_rss_l3_type(rss_type->ipv6, cmd);
		break;
	default:
		netdev_err(netdev, "Unsupported flow type\n");
		cmd->data = 0;
		return -EINVAL;
	}

	return 0;
}

static int hinic3_get_rss_hash_opts(struct net_device *netdev,
				    struct ethtool_rxnfc *cmd)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_rss_type rss_type;
	int err;

	cmd->data = 0;

	if (!test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags))
		return 0;

	err = hinic3_get_rss_type(nic_dev->hwdev, &rss_type);
	if (err) {
		netdev_err(netdev, "Failed to get rss type\n");
		return err;
	}

	return hinic3_convert_rss_type(netdev, &rss_type, cmd);
}

int hinic3_get_rxnfc(struct net_device *netdev,
		     struct ethtool_rxnfc *cmd, u32 *rule_locs)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err = 0;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = nic_dev->q_params.num_qps;
		break;
	case ETHTOOL_GRXFH:
		err = hinic3_get_rss_hash_opts(netdev, cmd);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

int hinic3_set_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd)
{
	int err;

	switch (cmd->cmd) {
	case ETHTOOL_SRXFH:
		err = hinic3_set_rss_hash_opts(netdev, cmd);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static u16 hinic3_max_channels(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u8 tcs = netdev_get_num_tc(netdev);

	return tcs ? nic_dev->max_qps / tcs : nic_dev->max_qps;
}

static u16 hinic3_curr_channels(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	if (netif_running(netdev))
		return nic_dev->q_params.num_qps ?
				nic_dev->q_params.num_qps : 1;
	else
		return min_t(u16, hinic3_max_channels(netdev),
			     nic_dev->q_params.num_qps);
}

void hinic3_get_channels(struct net_device *netdev,
			 struct ethtool_channels *channels)
{
	channels->max_rx = 0;
	channels->max_tx = 0;
	channels->max_other = 0;
	/* report maximum channels */
	channels->max_combined = hinic3_max_channels(netdev);
	channels->rx_count = 0;
	channels->tx_count = 0;
	channels->other_count = 0;
	/* report flow director queues as maximum channels */
	channels->combined_count = hinic3_curr_channels(netdev);
}

static int
hinic3_validate_channel_parameter(struct net_device *netdev,
				  const struct ethtool_channels *channels)
{
	u16 max_channel = hinic3_max_channels(netdev);
	unsigned int count = channels->combined_count;

	if (!count) {
		netdev_err(netdev, "Unsupported combined_count=0\n");
		return -EINVAL;
	}

	if (channels->tx_count || channels->rx_count || channels->other_count) {
		netdev_err(netdev, "Setting rx/tx/other count not supported\n");
		return -EINVAL;
	}

	if (count > max_channel) {
		netdev_err(netdev, "Combined count %u exceed limit %u\n", count,
			   max_channel);
		return -EINVAL;
	}

	return 0;
}

static int hinic3_rss_update_num_qps_and_reprogram(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	if (!netif_is_rxfh_configured(netdev))
		hinic3_rss_set_indir_tbl(nic_dev->hwdev, nic_dev->rss_indir);

	if (!netif_running(netdev))
		return 0;

	err = hinic3_set_hw_rss_parameters(netdev, 1);
	if (err)
		netdev_err(netdev,
			   "Failed to update RSS parameters after changing channels\n");

	return err;
}

int hinic3_set_channels(struct net_device *netdev,
			struct ethtool_channels *channels)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	unsigned int count = channels->combined_count;
	struct hinic3_dyna_txrxq_params q_params;
	int err;

	err = hinic3_validate_channel_parameter(netdev, channels);
	if (err)
		return err;

	if (!test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags)) {
		netdev_err(netdev, "This function doesn't support RSS, only support 1 queue pair\n");
		return -EOPNOTSUPP;
	}

	netdev_dbg(netdev, "Set max combined queue number from %u to %u\n",
		   nic_dev->q_params.num_qps, count);

	if (netif_running(netdev)) {
		q_params = nic_dev->q_params;
		q_params.num_qps = (u16)count;
		q_params.txqs_res = NULL;
		q_params.rxqs_res = NULL;
		q_params.irq_cfg = NULL;

		err = hinic3_change_channel_settings(netdev, &q_params);
		if (err) {
			netdev_err(netdev, "Failed to change channel settings\n");
			return err;
		}
	} else {
		nic_dev->q_params.num_qps = (u16)count;
	}

	err = hinic3_rss_update_num_qps_and_reprogram(netdev);
	if (err)
		return err;

	return 0;
}

u32 hinic3_get_rxfh_indir_size(struct net_device *netdev)
{
	return L2NIC_RSS_INDIR_SIZE;
}

static int hinic3_set_rss_rxfh(struct net_device *netdev,
			       const u32 *indir, u8 *key)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 temp_indir[L2NIC_RSS_INDIR_SIZE];
	int err;
	u32 i;

	if (indir) {
		for (i = 0; i < L2NIC_RSS_INDIR_SIZE; i++)
			temp_indir[i] = (u16)indir[i];

		err = hinic3_rss_set_indir_tbl(nic_dev->hwdev, temp_indir);
		if (err) {
			netdev_err(netdev, "Failed to set rss indir table\n");
			return err;
		}

		memcpy(nic_dev->rss_indir, temp_indir, L2NIC_RSS_INDIR_SIZE);
	}

	if (key) {
		err = hinic3_rss_set_hash_key(nic_dev->hwdev, key);
		if (err) {
			netdev_err(netdev, "Failed to set rss key\n");
			return err;
		}

		memcpy(nic_dev->rss_hkey, key, L2NIC_RSS_KEY_SIZE);
	}

	return 0;
}

u32 hinic3_get_rxfh_key_size(struct net_device *netdev)
{
	return L2NIC_RSS_KEY_SIZE;
}

static int hinic3_rss_get_indir_tbl(struct hinic3_hwdev *hwdev,
				    u32 *indir_table)
{
	struct hinic3_cmd_buf_pair pair;
	__le16 *indir_tbl = NULL;
	int err, i;

	err = hinic3_cmd_buf_pair_init(hwdev, &pair);
	if (err) {
		dev_err(hwdev->dev, "Failed to allocate cmd_buf.\n");
		return err;
	}

	memset(pair.in->buf, 0, pair.in->size);

	err = hinic3_cmdq_detail_resp(hwdev, MGMT_MOD_L2NIC,
				      L2NIC_UCODE_CMD_GET_RSS_INDIR_TBL,
				      pair.in, pair.out, NULL);
	if (err) {
		dev_err(hwdev->dev, "Failed to get rss indir table\n");
		goto err_get_indir_tbl;
	}

	indir_tbl = (__le16 *)pair.out->buf;
	for (i = 0; i < L2NIC_RSS_INDIR_SIZE; i++)
		indir_table[i] = le16_to_cpu(*(indir_tbl + i));

err_get_indir_tbl:
	hinic3_cmd_buf_pair_uninit(hwdev, &pair);

	return err;
}

int hinic3_get_rxfh(struct net_device *netdev,
		    struct ethtool_rxfh_param *rxfh)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err = 0;

	if (!test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags)) {
		netdev_err(netdev, "Rss is disabled\n");
		return -EOPNOTSUPP;
	}

	rxfh->hfunc =
		nic_dev->rss_hash_type == HINIC3_RSS_HASH_ENGINE_TYPE_XOR ?
		ETH_RSS_HASH_XOR : ETH_RSS_HASH_TOP;

	if (rxfh->indir) {
		err = hinic3_rss_get_indir_tbl(nic_dev->hwdev, rxfh->indir);
		if (err)
			return err;
	}

	if (rxfh->key)
		memcpy(rxfh->key, nic_dev->rss_hkey, L2NIC_RSS_KEY_SIZE);

	return err;
}

static int hinic3_update_hash_func_type(struct net_device *netdev, u8 hfunc)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	enum hinic3_rss_hash_type new_rss_hash_type;
	int err;

	switch (hfunc) {
	case ETH_RSS_HASH_NO_CHANGE:
		return 0;
	case ETH_RSS_HASH_XOR:
		new_rss_hash_type = HINIC3_RSS_HASH_ENGINE_TYPE_XOR;
		break;
	case ETH_RSS_HASH_TOP:
		new_rss_hash_type = HINIC3_RSS_HASH_ENGINE_TYPE_TOEP;
		break;
	default:
		netdev_err(netdev, "Unsupported hash func %u\n", hfunc);
		return -EOPNOTSUPP;
	}

	if (new_rss_hash_type == nic_dev->rss_hash_type)
		return 0;

	err = hinic3_rss_set_hash_type(nic_dev->hwdev, nic_dev->rss_hash_type);
	if (err) {
		netdev_err(netdev, "Failed to set RSS hash type to HW\n");
		return err;
	}

	nic_dev->rss_hash_type = new_rss_hash_type;

	return 0;
}

int hinic3_set_rxfh(struct net_device *netdev,
		    struct ethtool_rxfh_param *rxfh,
		    struct netlink_ext_ack *extack)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	if (!test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags)) {
		netdev_err(netdev, "Not support to set rss parameters when rss is disable\n");
		return -EOPNOTSUPP;
	}

	err = hinic3_update_hash_func_type(netdev, rxfh->hfunc);
	if (err)
		return err;

	err = hinic3_set_rss_rxfh(netdev, rxfh->indir, rxfh->key);

	return err;
}
