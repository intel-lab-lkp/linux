/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef __INTEL_DARKSCREEN_H__
#define __INTEL_DARKSCREEN_H__

#include <linux/types.h>
#include <linux/workqueue.h>

struct intel_crtc;

struct intel_darkscreen {
	bool enable;
	struct work_struct darkscreen_detect_work;
	struct intel_crtc *crtc;
};

void intel_darkscreen_setup(struct intel_crtc *crtc);
int intel_darkscreen_enable(struct intel_crtc *crtc);
void intel_darkscreen_disable(struct intel_crtc *crtc);
void intel_darkscreen_schedule_work(struct intel_crtc *crtc);

#endif /* __INTEL_DARKSCREEN_H_ */
