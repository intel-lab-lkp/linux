// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include "hinic3_mgmt.h"
#include "hinic3_hwdev.h"
#include "hinic3_eqs.h"
#include "hinic3_mbox.h"

void hinic3_flush_mgmt_workq(struct hinic3_hwdev *hwdev)
{
	if (hwdev->aeqs)
		flush_workqueue(hwdev->aeqs->workq);
}

void hinic3_mgmt_msg_aeqe_handler(struct hinic3_hwdev *hwdev, u8 *header, u8 size)
{
	if ((HINIC3_MSG_HEADER_GET(*(u64 *)header, SOURCE) ==
	     HINIC3_MSG_FROM_MBOX)) {
		hinic3_mbox_func_aeqe_handler(hwdev, header, size);
	}
}
