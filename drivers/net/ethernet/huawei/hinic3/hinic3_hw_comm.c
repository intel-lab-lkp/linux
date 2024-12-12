// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/delay.h>

#include "hinic3_hw_comm.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
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
