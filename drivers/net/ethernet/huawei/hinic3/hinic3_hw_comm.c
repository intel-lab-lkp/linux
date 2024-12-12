// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/delay.h>

#include "hinic3_hw_comm.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
#include "hinic3_cmdq.h"
#include "hinic3_hwif.h"

static int comm_msg_to_mgmt_sync(struct hinic3_hwdev *hwdev, u16 cmd, const void *buf_in,
				 u32 in_size, void *buf_out, u32 *out_size)
{
	return hinic3_send_mbox_to_mgmt(hwdev, HINIC3_MOD_COMM, cmd, buf_in,
					in_size, buf_out, out_size, 0);
}

int hinic3_set_interrupt_cfg_direct(struct hinic3_hwdev *hwdev,
				    const struct interrupt_info *info)
{
	struct comm_cmd_msix_config msix_cfg;
	u32 out_size = sizeof(msix_cfg);
	int err;

	memset(&msix_cfg, 0, sizeof(msix_cfg));
	msix_cfg.func_id = hinic3_global_func_id(hwdev);
	msix_cfg.msix_index = info->msix_index;
	msix_cfg.opcode = MGMT_MSG_CMD_OP_SET;

	msix_cfg.lli_credit_cnt = info->lli_credit_limit;
	msix_cfg.lli_timer_cnt = info->lli_timer_cfg;
	msix_cfg.pending_cnt = info->pending_limt;
	msix_cfg.coalesce_timer_cnt = info->coalesc_timer_cfg;
	msix_cfg.resend_timer_cnt = info->resend_timer_cfg;

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_CFG_MSIX_CTRL_REG,
				    &msix_cfg, sizeof(msix_cfg), &msix_cfg,
				    &out_size);
	if (err || !out_size || msix_cfg.head.status) {
		dev_err(hwdev->dev,
			"Failed to set interrupt config, err: %d, status: 0x%x, out size: 0x%x\n",
			err, msix_cfg.head.status, out_size);
		return -EINVAL;
	}

	return 0;
}

int hinic3_func_reset(struct hinic3_hwdev *hwdev, u16 func_id, u64 reset_flag)
{
	struct comm_cmd_func_reset func_reset;
	u32 out_size = sizeof(func_reset);
	int err;

	memset(&func_reset, 0, sizeof(func_reset));
	func_reset.func_id = func_id;
	func_reset.reset_flag = reset_flag;
	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_FUNC_RESET,
				    &func_reset, sizeof(func_reset),
				    &func_reset, &out_size);
	if (err || !out_size || func_reset.head.status) {
		dev_err(hwdev->dev, "Failed to reset func resources, reset_flag 0x%llx, err: %d, status: 0x%x, out_size: 0x%x\n",
			reset_flag, err, func_reset.head.status, out_size);
		return -EIO;
	}

	return 0;
}

static int hinic3_comm_features_nego(struct hinic3_hwdev *hwdev, u8 opcode, u64 *s_feature,
				     u16 size)
{
	struct comm_cmd_feature_nego feature_nego;
	u32 out_size = sizeof(feature_nego);
	int err;

	memset(&feature_nego, 0, sizeof(feature_nego));
	feature_nego.func_id = hinic3_global_func_id(hwdev);
	feature_nego.opcode = opcode;
	if (opcode == MGMT_MSG_CMD_OP_SET)
		memcpy(feature_nego.s_feature, s_feature, (size * sizeof(u64)));

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_FEATURE_NEGO,
				    &feature_nego, sizeof(feature_nego),
				    &feature_nego, &out_size);
	if (err || !out_size || feature_nego.head.status) {
		dev_err(hwdev->dev, "Failed to negotiate feature, err: %d, status: 0x%x, out size: 0x%x\n",
			err, feature_nego.head.status, out_size);
		return -EINVAL;
	}

	if (opcode == MGMT_MSG_CMD_OP_GET)
		memcpy(s_feature, feature_nego.s_feature, (size * sizeof(u64)));

	return 0;
}

int hinic3_get_comm_features(struct hinic3_hwdev *hwdev, u64 *s_feature, u16 size)
{
	return hinic3_comm_features_nego(hwdev, MGMT_MSG_CMD_OP_GET, s_feature,
					 size);
}

int hinic3_set_comm_features(struct hinic3_hwdev *hwdev, u64 *s_feature, u16 size)
{
	return hinic3_comm_features_nego(hwdev, MGMT_MSG_CMD_OP_SET, s_feature,
					 size);
}

int hinic3_get_global_attr(struct hinic3_hwdev *hwdev, struct comm_global_attr *attr)
{
	struct comm_cmd_get_glb_attr get_attr;
	u32 out_size = sizeof(get_attr);
	int err;

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_GET_GLOBAL_ATTR,
				    &get_attr, sizeof(get_attr), &get_attr,
				    &out_size);
	if (err || !out_size || get_attr.head.status) {
		dev_err(hwdev->dev,
			"Failed to get global attribute, err: %d, status: 0x%x, out size: 0x%x\n",
			err, get_attr.head.status, out_size);
		return -EIO;
	}

	memcpy(attr, &get_attr.attr, sizeof(*attr));

	return 0;
}

int hinic3_set_func_svc_used_state(struct hinic3_hwdev *hwdev, u16 svc_type, u8 state)
{
	struct comm_cmd_func_svc_used_state used_state;
	u32 out_size = sizeof(used_state);
	int err;

	memset(&used_state, 0, sizeof(used_state));
	used_state.func_id = hinic3_global_func_id(hwdev);
	used_state.svc_type = svc_type;
	used_state.used_state = state;

	err = comm_msg_to_mgmt_sync(hwdev,
				    COMM_MGMT_CMD_SET_FUNC_SVC_USED_STATE,
				    &used_state, sizeof(used_state),
				    &used_state, &out_size);
	if (err || !out_size || used_state.head.status) {
		dev_err(hwdev->dev,
			"Failed to set func service used state, err: %d, status: 0x%x, out size: 0x%x\n",
			err, used_state.head.status, out_size);
		return -EIO;
	}

	return 0;
}

int hinic3_set_dma_attr_tbl(struct hinic3_hwdev *hwdev, u8 entry_idx, u8 st, u8 at, u8 ph,
			    u8 no_snooping, u8 tph_en)
{
	struct comm_cmd_dma_attr_config dma_attr;
	u32 out_size = sizeof(dma_attr);
	int err;

	memset(&dma_attr, 0, sizeof(dma_attr));
	dma_attr.func_id = hinic3_global_func_id(hwdev);
	dma_attr.entry_idx = entry_idx;
	dma_attr.st = st;
	dma_attr.at = at;
	dma_attr.ph = ph;
	dma_attr.no_snooping = no_snooping;
	dma_attr.tph_en = tph_en;

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_SET_DMA_ATTR, &dma_attr, sizeof(dma_attr),
				    &dma_attr, &out_size);
	if (err || !out_size || dma_attr.head.status) {
		dev_err(hwdev->dev, "Failed to set dma attr, err: %d, status: 0x%x, out_size: 0x%x\n",
			err, dma_attr.head.status, out_size);
		return -EIO;
	}

	return 0;
}

int hinic3_set_wq_page_size(struct hinic3_hwdev *hwdev, u16 func_idx, u32 page_size)
{
	struct comm_cmd_wq_page_size page_size_info;
	u32 out_size = sizeof(page_size_info);
	int err;

	memset(&page_size_info, 0, sizeof(page_size_info));
	page_size_info.func_id = func_idx;
	page_size_info.page_size = ilog2(page_size / HINIC3_MIN_PAGE_SIZE);
	page_size_info.opcode = MGMT_MSG_CMD_OP_SET;

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_CFG_PAGESIZE,
				    &page_size_info, sizeof(page_size_info),
				    &page_size_info, &out_size);
	if (err || !out_size || page_size_info.head.status) {
		dev_err(hwdev->dev,
			"Failed to set wq page size, err: %d, status: 0x%x, out_size: 0x%0x\n",
			err, page_size_info.head.status, out_size);
		return -EFAULT;
	}

	return 0;
}

int hinic3_set_cmdq_depth(struct hinic3_hwdev *hwdev, u16 cmdq_depth)
{
	struct comm_cmd_root_ctxt root_ctxt;
	u32 out_size = sizeof(root_ctxt);
	int err;

	memset(&root_ctxt, 0, sizeof(root_ctxt));
	root_ctxt.func_id = hinic3_global_func_id(hwdev);

	root_ctxt.set_cmdq_depth = 1;
	root_ctxt.cmdq_depth = ilog2(cmdq_depth);

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_SET_VAT, &root_ctxt,
				    sizeof(root_ctxt), &root_ctxt, &out_size);
	if (err || !out_size || root_ctxt.head.status) {
		dev_err(hwdev->dev,
			"Failed to set cmdq depth, err: %d, status: 0x%x, out_size: 0x%x\n",
			err, root_ctxt.head.status, out_size);
		return -EFAULT;
	}

	return 0;
}

#define HINIC3_WAIT_CMDQ_IDLE_TIMEOUT    5000

static enum hinic3_wait_return check_cmdq_stop_handler(void *priv_data)
{
	struct hinic3_hwdev *hwdev = priv_data;
	enum hinic3_cmdq_type cmdq_type;
	struct hinic3_cmdqs *cmdqs;

	cmdqs = hwdev->cmdqs;
	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++) {
		if (!hinic3_cmdq_idle(&cmdqs->cmdq[cmdq_type]))
			return WAIT_PROCESS_WAITING;
	}

	return WAIT_PROCESS_CPL;
}

static int wait_cmdq_stop(struct hinic3_hwdev *hwdev)
{
	struct hinic3_cmdqs *cmdqs = hwdev->cmdqs;
	enum hinic3_cmdq_type cmdq_type;
	int err;

	if (!(cmdqs->status & HINIC3_CMDQ_ENABLE))
		return 0;

	cmdqs->status &= ~HINIC3_CMDQ_ENABLE;
	err = hinic3_wait_for_timeout(hwdev, check_cmdq_stop_handler,
				      HINIC3_WAIT_CMDQ_IDLE_TIMEOUT,
				      USEC_PER_MSEC);
	if (!err)
		return 0;

	for (cmdq_type = 0; cmdq_type < cmdqs->cmdq_num; cmdq_type++) {
		if (!hinic3_cmdq_idle(&cmdqs->cmdq[cmdq_type]))
			dev_err(hwdev->dev, "Cmdq %d is busy\n", cmdq_type);
	}
	cmdqs->status |= HINIC3_CMDQ_ENABLE;
	return err;
}

int hinic3_func_rx_tx_flush(struct hinic3_hwdev *hwdev)
{
	struct comm_cmd_clear_doorbell clear_db;
	struct comm_cmd_clear_resource clr_res;
	struct hinic3_hwif *hwif = hwdev->hwif;
	u32 out_size = sizeof(clear_db);
	int ret = 0;
	int err;

	err = wait_cmdq_stop(hwdev);
	if (err) {
		dev_warn(hwdev->dev, "CMDQ is still working, CMDQ timeout value is unreasonable\n");
		ret = err;
	}

	hinic3_disable_doorbell(hwif);

	memset(&clear_db, 0, sizeof(clear_db));
	clear_db.func_id = hwif->attr.func_global_idx;
	err = comm_msg_to_mgmt_sync(hwdev,  COMM_MGMT_CMD_FLUSH_DOORBELL,
				    &clear_db, sizeof(clear_db),
				    &clear_db, &out_size);
	if (err || !out_size || clear_db.head.status) {
		dev_warn(hwdev->dev, "Failed to flush doorbell, err: %d, status: 0x%x, out_size: 0x%x\n",
			 err, clear_db.head.status, out_size);
		if (err)
			ret = err;
		else
			ret = -EFAULT;
	}

	/* wait for chip to stop I/O */
	msleep(100);

	memset(&clr_res, 0, sizeof(clr_res));
	clr_res.func_id = hwif->attr.func_global_idx;
	err = hinic3_send_mbox_to_mgmt_no_ack(hwdev, HINIC3_MOD_COMM,
					      COMM_MGMT_CMD_START_FLUSH,
					      &clr_res, sizeof(clr_res));
	if (err) {
		dev_warn(hwdev->dev, "Failed to notice flush message, err: %d\n",
			 err);
		ret = err;
	}

	hinic3_enable_doorbell(hwif);

	err = hinic3_reinit_cmdq_ctxts(hwdev);
	if (err) {
		dev_warn(hwdev->dev, "Failed to reinit cmdq\n");
		ret = err;
	}

	return ret;
}

static int get_hw_rx_buf_size_idx(int rx_buf_sz, u16 *buf_sz_idx)
{
	/* Supported RX buffer sizes in bytes. Configured by array index. */
	static const int supported_sizes[16] = {
		[0] = 32,     [1] = 64,     [2] = 96,     [3] = 128,
		[4] = 192,    [5] = 256,    [6] = 384,    [7] = 512,
		[8] = 768,    [9] = 1024,   [10] = 1536,  [11] = 2048,
		[12] = 3072,  [13] = 4096,  [14] = 8192,  [15] = 16384,
	};
	u16 idx;

	/* Scan from biggest to smallest. Choose supported size that is equal or
	 * smaller. For smaller value HW will under-utilize posted buffers. For
	 * bigger value HW may overrun posted buffers.
	 */
	idx = ARRAY_SIZE(supported_sizes);
	while (idx > 0) {
		idx--;
		if (supported_sizes[idx] <= rx_buf_sz) {
			*buf_sz_idx = idx;
			return 0;
		}
	}

	return -EINVAL;
}

int hinic3_set_root_ctxt(struct hinic3_hwdev *hwdev, u32 rq_depth, u32 sq_depth, int rx_buf_sz)
{
	struct comm_cmd_root_ctxt root_ctxt;
	u32 out_size = sizeof(root_ctxt);
	u16 buf_sz_idx;
	int err;

	err = get_hw_rx_buf_size_idx(rx_buf_sz, &buf_sz_idx);
	if (err)
		return err;

	memset(&root_ctxt, 0, sizeof(root_ctxt));
	root_ctxt.func_id = hinic3_global_func_id(hwdev);

	root_ctxt.set_cmdq_depth = 0;
	root_ctxt.cmdq_depth = 0;

	root_ctxt.lro_en = 1;

	root_ctxt.rq_depth  = ilog2(rq_depth);
	root_ctxt.rx_buf_sz = buf_sz_idx;
	root_ctxt.sq_depth  = ilog2(sq_depth);

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_SET_VAT,
				    &root_ctxt, sizeof(root_ctxt),
				    &root_ctxt, &out_size);
	if (err || !out_size || root_ctxt.head.status) {
		dev_err(hwdev->dev,
			"Failed to set root context, err: %d, status: 0x%x, out_size: 0x%x\n",
			err, root_ctxt.head.status, out_size);
		return -EFAULT;
	}

	return 0;
}

int hinic3_clean_root_ctxt(struct hinic3_hwdev *hwdev)
{
	struct comm_cmd_root_ctxt root_ctxt;
	u32 out_size = sizeof(root_ctxt);
	int err;

	memset(&root_ctxt, 0, sizeof(root_ctxt));
	root_ctxt.func_id = hinic3_global_func_id(hwdev);

	err = comm_msg_to_mgmt_sync(hwdev, COMM_MGMT_CMD_SET_VAT,
				    &root_ctxt, sizeof(root_ctxt),
				    &root_ctxt, &out_size);
	if (err || !out_size || root_ctxt.head.status) {
		dev_err(hwdev->dev,
			"Failed to set root context, err: %d, status: 0x%x, out_size: 0x%x\n",
			err, root_ctxt.head.status, out_size);
		return -EFAULT;
	}

	return 0;
}
