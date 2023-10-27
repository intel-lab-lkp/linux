/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 */

#ifndef __INTEL_DARKSCREEN_H__
#define __INTEL_DARKSCREEN_H__

#include <linux/types.h>

struct intel_crtc_state;
struct intel_crtc;

struct intel_darkscreen {
	bool enable;
};

#ifdef CONFIG_DEBUG_FS
void intel_dark_screen_enable(struct intel_crtc_state *crtc_state);
void intel_dark_screen_disable(struct intel_crtc_state *crtc_state);
void intel_darkscreen_crtc_debugfs_add(struct intel_crtc *crtc);

#endif

#endif /* __INTEL_DARKSCREEN_H_ */
