/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_RESOURCE_GROUP_H__
#define __PANTHOR_RESOURCE_GROUP_H__

#include <linux/types.h>

struct device;
struct panthor_arbitration;

int panthor_resource_group_init(struct panthor_arbitration *adev);

void panthor_resource_group_term(struct panthor_arbitration *adev);

int panthor_resource_group_suspend(struct panthor_arbitration *adev);

int panthor_resource_group_resume(struct panthor_arbitration *adev);

#endif
