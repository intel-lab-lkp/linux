// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/device.h>
#include <linux/io.h>
#include <linux/bitfield.h>

#include "hinic3_hwdev.h"
#include "hinic3_common.h"
#include "hinic3_hwif.h"

u16 hinic3_global_func_id(struct hinic3_hwdev *hwdev)
{
	return hwdev->hwif->attr.func_global_idx;
}
