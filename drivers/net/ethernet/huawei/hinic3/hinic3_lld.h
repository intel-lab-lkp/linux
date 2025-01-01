/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_LLD_H
#define HINIC3_LLD_H

#include <linux/auxiliary_bus.h>

struct hinic3_event_info;

#define HINIC3_NIC_DRV_NAME "hinic3"

int hinic3_lld_init(void);
void hinic3_lld_exit(void);
int hinic3_adev_event_register(struct auxiliary_device *adev,
			       void (*event_handler)(struct auxiliary_device *adev,
						     struct hinic3_event_info *event));
struct hinic3_hwdev *adev_get_hwdev(struct auxiliary_device *adev);

#endif
