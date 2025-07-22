/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2025 Arm, Ltd. */
#ifndef __ETHOS_DRV_H__
#define __ETHOS_DRV_H__

#include <drm/gpu_scheduler.h>

struct ethos_device;

struct ethos_file_priv {
	struct ethos_device *edev;
	struct drm_sched_entity sched_entity;
};

#endif
