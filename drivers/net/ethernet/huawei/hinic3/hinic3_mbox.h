/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_MBOX_H
#define HINIC3_MBOX_H

#include <linux/mutex.h>
#include <linux/bitfield.h>

struct hinic3_hwdev;

int hinic3_send_mbox_to_mgmt(struct hinic3_hwdev *hwdev, u8 mod, u16 cmd,
			     const void *buf_in, u32 in_size, void *buf_out,
			     u32 *out_size, u32 timeout);

#endif
