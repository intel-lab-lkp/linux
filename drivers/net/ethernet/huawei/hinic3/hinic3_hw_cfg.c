// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/device.h>

#include "hinic3_hw_cfg.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
#include "hinic3_hwif.h"

#define IS_NIC_TYPE(hwdev) \
	(((u32)(hwdev)->cfg_mgmt->svc_cap.chip_svc_type) & BIT(SERVICE_T_NIC))

bool hinic3_support_nic(struct hinic3_hwdev *hwdev)
{
	if (!IS_NIC_TYPE(hwdev))
		return false;

	return true;
}

u16 hinic3_func_max_qnum(struct hinic3_hwdev *hwdev)
{
	return hwdev->cfg_mgmt->svc_cap.nic_cap.max_sqs;
}

u8 hinic3_physical_port_id(struct hinic3_hwdev *hwdev)
{
	return hwdev->cfg_mgmt->svc_cap.port_id;
}
