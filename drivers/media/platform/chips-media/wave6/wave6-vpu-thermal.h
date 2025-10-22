/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Wave6 series multi-standard codec IP - wave6 thermal cooling interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 *
 */

#ifndef __WAVE6_VPU_THERMAL_H__
#define __WAVE6_VPU_THERMAL_H__

#include <linux/thermal.h>

struct vpu_thermal_cooling {
	struct device *dev;
	int thermal_event;
	int thermal_max;
	struct thermal_cooling_device *cooling;
	unsigned long *freq_table;
};

int wave6_vpu_cooling_init(struct vpu_thermal_cooling *thermal);
void wave6_vpu_cooling_remove(struct vpu_thermal_cooling *thermal);

#endif /* __WAVE6_VPU_THERMAL_H__ */
