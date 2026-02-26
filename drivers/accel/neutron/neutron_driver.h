/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright 2025 NXP */

#ifndef __NEUTRON_DRIVER_H__
#define __NEUTRON_DRIVER_H__

#include <drm/gpu_scheduler.h>

struct neutron_device;

struct neutron_file_priv {
	struct neutron_device *ndev;
	struct drm_sched_entity sched_entity;
};

#endif /* __NEUTRON_DRIVER_H__ */
