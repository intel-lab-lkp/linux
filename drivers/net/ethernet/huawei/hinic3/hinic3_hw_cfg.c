// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include <linux/device.h>

#include "hinic3_hw_cfg.h"
#include "hinic3_hwdev.h"
#include "hinic3_hwif.h"
#include "hinic3_mbox.h"

bool hinic3_support_nic(struct hinic3_hwdev *hwdev)
{
	return ((u32)hwdev->cfg_mgmt->svc_cap.chip_svc_type &
		BIT(HINIC3_SERVICE_T_NIC)) == 0 ? false : true;
}

u16 hinic3_func_max_qnum(struct hinic3_hwdev *hwdev)
{
	return hwdev->cfg_mgmt->svc_cap.nic_cap.max_sqs;
}

u8 hinic3_physical_port_id(struct hinic3_hwdev *hwdev)
{
	return hwdev->cfg_mgmt->svc_cap.port_id;
}
