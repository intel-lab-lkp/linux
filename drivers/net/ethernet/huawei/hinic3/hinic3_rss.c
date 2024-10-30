// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/types.h>
#include <linux/errno.h>

#include "hinic3_cmdq.h"
#include "hinic3_mgmt_interface.h"
#include "hinic3_nic_dev.h"
#include "hinic3_nic_cfg.h"
#include "hinic3_rss.h"

static void hinic3_fillout_indir_tbl(struct net_device *netdev, u32 *indir)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u32 i, num_qps;

	num_qps = nic_dev->q_params.num_qps;
	for (i = 0; i < NIC_RSS_INDIR_SIZE; i++)
		indir[i] = i % num_qps;
}

static int hinic3_rss_cfg(struct hinic3_hwdev *hwdev, u8 rss_en, u16 num_qps)
{
	struct hinic3_cmd_rss_config rss_cfg;
	u32 out_size = sizeof(rss_cfg);
	int err;

	memset(&rss_cfg, 0, sizeof(struct hinic3_cmd_rss_config));
	rss_cfg.func_id = hinic3_global_func_id(hwdev);
	rss_cfg.rss_en = rss_en;
	rss_cfg.rq_priority_number = 0;
	rss_cfg.num_qps = num_qps;

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_RSS_CFG,
				     &rss_cfg, sizeof(rss_cfg),
				     &rss_cfg, &out_size);
	if (err || !out_size || rss_cfg.msg_head.status) {
		dev_err(hwdev->dev, "Failed to set rss cfg, err: %d, status: 0x%x, out size: 0x%x\n",
			err, rss_cfg.msg_head.status, out_size);
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

static void hinic3_set_default_rss_indir(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	set_bit(HINIC3_RSS_DEFAULT_INDIR, &nic_dev->flags);
}

static void hinic3_maybe_reconfig_rss_indir(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int i;

	for (i = 0; i < NIC_RSS_INDIR_SIZE; i++) {
		if (nic_dev->rss_indir[i] >= nic_dev->q_params.num_qps) {
			hinic3_set_default_rss_indir(netdev);
			break;
		}
	}
}

/* Get number of CPUs on same NUMA node of device. */
static unsigned int dev_num_cpus(struct device *dev)
{
	unsigned int i, num_cpus, num_node_cpus;
	int dev_node;

	dev_node = dev_to_node(dev);
	num_cpus = num_online_cpus();
	num_node_cpus = 0;

	for (i = 0; i < num_cpus; i++) {
		if (cpu_to_node(i) == dev_node)
			num_node_cpus++;
	}

	return num_node_cpus ? : num_cpus;
}

static void decide_num_qps(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	unsigned int dev_cpus, max_qps, num_qps;

	dev_cpus = dev_num_cpus(&nic_dev->pdev->dev);
	max_qps = min(dev_cpus, nic_dev->max_qps);
	num_qps = nic_dev->nic_cap.default_num_queues;
	if (num_qps == 0 || num_qps > max_qps)
		num_qps = max_qps;

	nic_dev->q_params.num_qps = num_qps;
}

static int alloc_rss_resource(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	static const u8 default_rss_key[NIC_RSS_KEY_SIZE] = {
		0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
		0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
		0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
		0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
		0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

	nic_dev->rss_hkey = kzalloc(NIC_RSS_KEY_SIZE, GFP_KERNEL);
	if (!nic_dev->rss_hkey)
		return -ENOMEM;

	memcpy(nic_dev->rss_hkey, default_rss_key, NIC_RSS_KEY_SIZE);

	nic_dev->rss_indir = kzalloc(sizeof(u32) * NIC_RSS_INDIR_SIZE, GFP_KERNEL);
	if (!nic_dev->rss_indir) {
		kfree(nic_dev->rss_hkey);
		nic_dev->rss_hkey = NULL;
		return -ENOMEM;
	}

	set_bit(HINIC3_RSS_DEFAULT_INDIR, &nic_dev->flags);

	return 0;
}

static int hinic3_rss_set_indir_tbl(struct hinic3_hwdev *hwdev,
				    const u32 *indir_table)
{
	struct nic_rss_indirect_tbl_set *indir_tbl;
	struct hinic3_cmd_buf *cmd_buf;
	u64 out_param;
	int err;
	u32 i;

	cmd_buf = hinic3_alloc_cmd_buf(hwdev);
	if (!cmd_buf) {
		dev_err(hwdev->dev, "Failed to allocate cmd buf\n");
		return -ENOMEM;
	}

	cmd_buf->size = sizeof(struct nic_rss_indirect_tbl_set);
	indir_tbl = cmd_buf->buf;
	memset(indir_tbl, 0, sizeof(*indir_tbl));

	for (i = 0; i < NIC_RSS_INDIR_SIZE; i++)
		indir_tbl->entry[i] = indir_table[i];

	cmdq_buf_swab32(indir_tbl, sizeof(*indir_tbl));

	err = hinic3_cmdq_direct_resp(hwdev, HINIC3_MOD_L2NIC,
				      HINIC3_UCODE_CMD_SET_RSS_INDIR_TABLE,
				      cmd_buf, &out_param);
	if (err || out_param != 0) {
		dev_err(hwdev->dev, "Failed to set rss indir table\n");
		err = -EFAULT;
	}

	hinic3_free_cmd_buf(hwdev, cmd_buf);
	return err;
}

static int hinic3_set_rss_type(struct hinic3_hwdev *hwdev,
			       struct nic_rss_type rss_type)
{
	struct hinic3_rss_context_table ctx_tbl;
	u32 out_size = sizeof(ctx_tbl);
	u32 ctx;
	int err;

	memset(&ctx_tbl, 0, sizeof(ctx_tbl));
	ctx_tbl.func_id = hinic3_global_func_id(hwdev);
	ctx = HINIC3_RSS_TYPE_SET(1, VALID) |
	      HINIC3_RSS_TYPE_SET(rss_type.ipv4, IPV4) |
	      HINIC3_RSS_TYPE_SET(rss_type.ipv6, IPV6) |
	      HINIC3_RSS_TYPE_SET(rss_type.ipv6_ext, IPV6_EXT) |
	      HINIC3_RSS_TYPE_SET(rss_type.tcp_ipv4, TCP_IPV4) |
	      HINIC3_RSS_TYPE_SET(rss_type.tcp_ipv6, TCP_IPV6) |
	      HINIC3_RSS_TYPE_SET(rss_type.tcp_ipv6_ext, TCP_IPV6_EXT) |
	      HINIC3_RSS_TYPE_SET(rss_type.udp_ipv4, UDP_IPV4) |
	      HINIC3_RSS_TYPE_SET(rss_type.udp_ipv6, UDP_IPV6);
	ctx_tbl.context = ctx;
	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_SET_RSS_CTX_TBL_INTO_FUNC,
				     &ctx_tbl, sizeof(ctx_tbl),
				     &ctx_tbl, &out_size);

	if (ctx_tbl.msg_head.status == MGMT_CMD_UNSUPPORTED) {
		return MGMT_CMD_UNSUPPORTED;
	} else if (err || !out_size || ctx_tbl.msg_head.status) {
		dev_err(hwdev->dev, "mgmt Failed to set rss context offload, err: %d, status: 0x%x, out size: 0x%x\n",
			err, ctx_tbl.msg_head.status, out_size);
		return -EINVAL;
	}

	return 0;
}

static int hinic3_get_rss_type(struct hinic3_hwdev *hwdev,
			       struct nic_rss_type *rss_type)
{
	struct hinic3_rss_context_table ctx_tbl;
	u32 out_size = sizeof(ctx_tbl);
	int err;

	memset(&ctx_tbl, 0, sizeof(struct hinic3_rss_context_table));
	ctx_tbl.func_id = hinic3_global_func_id(hwdev);

	err = l2nic_msg_to_mgmt_sync(hwdev, HINIC3_NIC_CMD_GET_RSS_CTX_TBL,
				     &ctx_tbl, sizeof(ctx_tbl),
				     &ctx_tbl, &out_size);
	if (err || !out_size || ctx_tbl.msg_head.status) {
		dev_err(hwdev->dev, "Failed to get hash type, err: %d, status: 0x%x, out size: 0x%x\n",
			err, ctx_tbl.msg_head.status, out_size);
			return -EINVAL;
	}

	rss_type->ipv4         = HINIC3_RSS_TYPE_GET(ctx_tbl.context, IPV4);
	rss_type->ipv6         = HINIC3_RSS_TYPE_GET(ctx_tbl.context, IPV6);
	rss_type->ipv6_ext     = HINIC3_RSS_TYPE_GET(ctx_tbl.context, IPV6_EXT);
	rss_type->tcp_ipv4     = HINIC3_RSS_TYPE_GET(ctx_tbl.context, TCP_IPV4);
	rss_type->tcp_ipv6     = HINIC3_RSS_TYPE_GET(ctx_tbl.context, TCP_IPV6);
	rss_type->tcp_ipv6_ext = HINIC3_RSS_TYPE_GET(ctx_tbl.context,
						     TCP_IPV6_EXT);
	rss_type->udp_ipv4     = HINIC3_RSS_TYPE_GET(ctx_tbl.context, UDP_IPV4);
	rss_type->udp_ipv6     = HINIC3_RSS_TYPE_GET(ctx_tbl.context, UDP_IPV6);

	return 0;
}

static int hinic3_rss_cfg_hash_type(struct hinic3_hwdev *hwdev, u8 opcode,
				    enum hinic3_rss_hash_type *type)
{
	struct hinic3_cmd_rss_engine_type hash_type_cmd;
	u32 out_size = sizeof(hash_type_cmd);
	int err;

	memset(&hash_type_cmd, 0, sizeof(struct hinic3_cmd_rss_engine_type));

	hash_type_cmd.func_id = hinic3_global_func_id(hwdev);
	hash_type_cmd.opcode = opcode;

	if (opcode == HINIC3_CMD_OP_SET)
		hash_type_cmd.hash_engine = *type;

	err = l2nic_msg_to_mgmt_sync(hwdev,
				     HINIC3_NIC_CMD_CFG_RSS_HASH_ENGINE,
				     &hash_type_cmd, sizeof(hash_type_cmd),
				     &hash_type_cmd, &out_size);
	if (err || !out_size || hash_type_cmd.msg_head.status) {
		dev_err(hwdev->dev, "Failed to %s hash engine, err: %d, status: 0x%x, out size: 0x%x\n",
			opcode == HINIC3_CMD_OP_SET ? "set" : "get",
			err, hash_type_cmd.msg_head.status, out_size);
		return -EIO;
	}

	if (opcode == HINIC3_CMD_OP_GET)
		*type = hash_type_cmd.hash_engine;

	return 0;
}

static int hinic3_rss_set_hash_type(struct hinic3_hwdev *hwdev,
				    enum hinic3_rss_hash_type type)
{
	return hinic3_rss_cfg_hash_type(hwdev, HINIC3_CMD_OP_SET, &type);
}

static int hinic3_config_rss_hw_resource(struct net_device *netdev,
					 u32 *indir_tbl)
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
	struct hinic3_cmd_rss_hash_key hash_key;
	u32 out_size = sizeof(hash_key);
	int err;

	memset(&hash_key, 0, sizeof(struct hinic3_cmd_rss_hash_key));
	hash_key.func_id = hinic3_global_func_id(hwdev);
	hash_key.opcode = opcode;

	if (opcode == HINIC3_CMD_OP_SET)
		memcpy(hash_key.key, key, NIC_RSS_KEY_SIZE);

	err = l2nic_msg_to_mgmt_sync(hwdev,
				     HINIC3_NIC_CMD_CFG_RSS_HASH_KEY,
				     &hash_key, sizeof(hash_key),
				     &hash_key, &out_size);
	if (err || !out_size || hash_key.msg_head.status) {
		dev_err(hwdev->dev, "Failed to %s hash key, err: %d, status: 0x%x, out size: 0x%x\n",
			opcode == HINIC3_CMD_OP_SET ? "set" : "get",
			err, hash_key.msg_head.status, out_size);
		return -EINVAL;
	}

	if (opcode == HINIC3_CMD_OP_GET)
		memcpy(key, hash_key.key, NIC_RSS_KEY_SIZE);

	return 0;
}

static int hinic3_rss_set_hash_key(struct hinic3_hwdev *hwdev, const u8 *key)
{
	u8 hash_key[NIC_RSS_KEY_SIZE];

	memcpy(hash_key, key, NIC_RSS_KEY_SIZE);
	return hinic3_rss_cfg_hash_key(hwdev, HINIC3_CMD_OP_SET, hash_key);
}

static int hinic3_set_hw_rss_parameters(struct net_device *netdev, u8 rss_en)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	err = hinic3_rss_set_hash_key(nic_dev->hwdev, nic_dev->rss_hkey);
	if (err)
		return err;

	hinic3_maybe_reconfig_rss_indir(netdev);

	if (test_bit(HINIC3_RSS_DEFAULT_INDIR, &nic_dev->flags))
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

void hinic3_rss_deinit(struct net_device *netdev)
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
	if (nic_dev->max_qps <= 1 || !hinic3_test_support(nic_dev, NIC_F_RSS))
		goto set_q_params;

	err = alloc_rss_resource(netdev);
	if (err) {
		nic_dev->max_qps = 1;
		goto set_q_params;
	}

	set_bit(HINIC3_RSS_ENABLE, &nic_dev->flags);
	nic_dev->max_qps = hinic3_func_max_qnum(hwdev);

	decide_num_qps(netdev);

	hinic3_init_rss_parameters(netdev);
	err = hinic3_set_hw_rss_parameters(netdev, 0);
	if (err) {
		dev_err(hwdev->dev, "Failed to set hardware rss parameters\n");

		hinic3_clear_rss_config(netdev);
		nic_dev->max_qps = 1;
		goto set_q_params;
	}
	return;

set_q_params:
	clear_bit(HINIC3_RSS_ENABLE, &nic_dev->flags);
	nic_dev->q_params.num_qps = nic_dev->max_qps;
}

static int set_l4_rss_hash_ops(const struct ethtool_rxnfc *cmd,
			       struct nic_rss_type *rss_type)
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

static int update_rss_hash_opts(struct net_device *netdev,
				struct ethtool_rxnfc *cmd,
				struct nic_rss_type *rss_type)
{
	int err;

	switch (cmd->flow_type) {
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
	case UDP_V4_FLOW:
	case UDP_V6_FLOW:
		err = set_l4_rss_hash_ops(cmd, rss_type);
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

static int hinic3_set_rss_hash_opts(struct net_device *netdev, struct ethtool_rxnfc *cmd)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct nic_rss_type *rss_type = &nic_dev->rss_type;
	int err;

	if (!test_bit(HINIC3_RSS_ENABLE, &nic_dev->flags)) {
		cmd->data = 0;
		netdev_err(netdev, "RSS is disable, not support to set flow-hash\n");
		return -EOPNOTSUPP;
	}

	/* RSS only supports hashing of IP addresses and L4 ports */
	if (cmd->data & ~(RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 |
		RXH_L4_B_2_3))
		return -EINVAL;

	/* Both IP addresses must be part of the hash tuple */
	if (!(cmd->data & RXH_IP_SRC) || !(cmd->data & RXH_IP_DST))
		return -EINVAL;

	err = hinic3_get_rss_type(nic_dev->hwdev, rss_type);
	if (err) {
		netdev_err(netdev, "Failed to get rss type\n");
		return -EFAULT;
	}

	err = update_rss_hash_opts(netdev, cmd, rss_type);
	if (err)
		return err;

	err = hinic3_set_rss_type(nic_dev->hwdev, *rss_type);
	if (err) {
		netdev_err(netdev, "Failed to set rss type\n");
		return -EFAULT;
	}

	return 0;
}

static void convert_rss_type(u8 rss_opt, struct ethtool_rxnfc *cmd)
{
	if (rss_opt)
		cmd->data |= RXH_L4_B_0_1 | RXH_L4_B_2_3;
}

static int hinic3_convert_rss_type(struct net_device *netdev,
				   struct nic_rss_type *rss_type,
				   struct ethtool_rxnfc *cmd)
{
	cmd->data = RXH_IP_SRC | RXH_IP_DST;
	switch (cmd->flow_type) {
	case TCP_V4_FLOW:
		convert_rss_type(rss_type->tcp_ipv4, cmd);
		break;
	case TCP_V6_FLOW:
		convert_rss_type(rss_type->tcp_ipv6, cmd);
		break;
	case UDP_V4_FLOW:
		convert_rss_type(rss_type->udp_ipv4, cmd);
		break;
	case UDP_V6_FLOW:
		convert_rss_type(rss_type->udp_ipv6, cmd);
		break;
	case IPV4_FLOW:
	case IPV6_FLOW:
		break;
	default:
		netdev_err(netdev, "Unsupported flow type\n");
		cmd->data = 0;
		return -EINVAL;
	}

	return 0;
}

static int hinic3_get_rss_hash_opts(struct net_device *netdev, struct ethtool_rxnfc *cmd)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct nic_rss_type rss_type;
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

static int hinic3_validate_channel_parameter(struct net_device *netdev,
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

static void change_num_channel_reopen_handler(struct net_device *netdev)
{
	hinic3_set_default_rss_indir(netdev);
}

int hinic3_set_channels(struct net_device *netdev,
			struct ethtool_channels *channels)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	unsigned int count = channels->combined_count;
	struct hinic3_dyna_txrxq_params q_params;
	int err;

	if (hinic3_validate_channel_parameter(netdev, channels))
		return -EINVAL;

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

		err = hinic3_change_channel_settings(netdev, &q_params,
						     change_num_channel_reopen_handler);
		if (err) {
			netdev_err(netdev, "Failed to change channel settings\n");
			return -EFAULT;
		}
	} else {
		/* Discard user configured rss */
		hinic3_set_default_rss_indir(netdev);
		nic_dev->q_params.num_qps = (u16)count;
	}

	return 0;
}

u32 hinic3_get_rxfh_indir_size(struct net_device *netdev)
{
	return NIC_RSS_INDIR_SIZE;
}

static int set_rss_rxfh(struct net_device *netdev, const u32 *indir,
			const u8 *key)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;

	if (indir) {
		err = hinic3_rss_set_indir_tbl(nic_dev->hwdev, indir);
		if (err) {
			netdev_err(netdev, "Failed to set rss indir table\n");
			return -EFAULT;
		}
		clear_bit(HINIC3_RSS_DEFAULT_INDIR, &nic_dev->flags);

		memcpy(nic_dev->rss_indir, indir, sizeof(u32) * NIC_RSS_INDIR_SIZE);
	}

	if (key) {
		err = hinic3_rss_set_hash_key(nic_dev->hwdev, key);
		if (err) {
			netdev_err(netdev, "Failed to set rss key\n");
			return -EFAULT;
		}

		memcpy(nic_dev->rss_hkey, key, NIC_RSS_KEY_SIZE);
	}

	return 0;
}

u32 hinic3_get_rxfh_key_size(struct net_device *netdev)
{
	return NIC_RSS_KEY_SIZE;
}

static int hinic3_rss_get_indir_tbl(struct hinic3_hwdev *hwdev, u32 *indir_table)
{
	struct hinic3_cmd_buf_pair pair;
	u16 *indir_tbl;
	int err, i;

	err = hinic3_cmd_buf_pair_init(hwdev, &pair);
	if (err) {
		dev_err(hwdev->dev, "Failed to allocate cmd_buf.\n");
		return -ENOMEM;
	}

	err = hinic3_cmdq_detail_resp(hwdev, HINIC3_MOD_L2NIC,
				      HINIC3_UCODE_CMD_GET_RSS_INDIR_TABLE,
				      pair.in, pair.out, NULL);
	if (err) {
		dev_err(hwdev->dev, "Failed to get rss indir table\n");
		goto err_get_indir_tbl;
	}

	indir_tbl = ((struct nic_rss_indirect_tbl_get *)(pair.out->buf))->entry;
	for (i = 0; i < NIC_RSS_INDIR_SIZE; i++)
		indir_table[i] = *(indir_tbl + i);

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
			return -EFAULT;
	}

	if (rxfh->key)
		memcpy(rxfh->key, nic_dev->rss_hkey, NIC_RSS_KEY_SIZE);

	return err;
}

static int update_hash_func_type(struct net_device *netdev, u8 hfunc)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	enum hinic3_rss_hash_type new_rss_hash_type;

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

	nic_dev->rss_hash_type = new_rss_hash_type;
	return hinic3_rss_set_hash_type(nic_dev->hwdev, nic_dev->rss_hash_type);
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

	err = update_hash_func_type(netdev, rxfh->hfunc);
	if (err)
		return err;

	err = set_rss_rxfh(netdev, rxfh->indir, rxfh->key);

	return err;
}
