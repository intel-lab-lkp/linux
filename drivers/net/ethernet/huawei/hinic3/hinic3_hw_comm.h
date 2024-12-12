/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_HW_COMM_H
#define HINIC3_HW_COMM_H

#include "hinic3_hw_intf.h"

struct hinic3_hwdev;

struct interrupt_info {
	u32 lli_set;
	u32 interrupt_coalesc_set;
	u16 msix_index;
	u8  lli_credit_limit;
	u8  lli_timer_cfg;
	u8  pending_limt;
	u8  coalesc_timer_cfg;
	u8  resend_timer_cfg;
};

int hinic3_set_interrupt_cfg_direct(struct hinic3_hwdev *hwdev,
				    const struct interrupt_info *info);
int hinic3_func_reset(struct hinic3_hwdev *hwdev, u16 func_id, u64 reset_flag);

#endif
