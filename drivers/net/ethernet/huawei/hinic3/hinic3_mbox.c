// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/dma-mapping.h>

#include "hinic3_mbox.h"
#include "hinic3_common.h"
#include "hinic3_hwdev.h"
#include "hinic3_hwif.h"

int hinic3_send_mbox_to_mgmt(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
			     const void *buf_in, u32 in_size, void *buf_out,
			     u32 *out_size, u32 timeout)
{
	/* Completed by later submission due to LoC limit. */
	return -EFAULT;
}
