// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include "hinic3_rss.h"
#include "hinic3_hwdev.h"
#include "hinic3_nic_dev.h"
#include "hinic3_nic_cfg.h"
#include "hinic3_cmdq.h"
#include "hinic3_hwif.h"

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
	unsigned int dev_cpus;

	dev_cpus = dev_num_cpus(&nic_dev->pdev->dev);
	nic_dev->q_params.num_qps = min(dev_cpus, nic_dev->max_qps);
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
