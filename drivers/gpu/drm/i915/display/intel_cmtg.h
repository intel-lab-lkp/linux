/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2024 Intel Corporation
 */

#ifndef __INTEL_CMTG_H__
#define __INTEL_CMTG_H__

#include <linux/types.h>

struct intel_display;
struct intel_global_state;

int intel_cmtg_init(struct intel_display *display);
void intel_cmtg_readout_hw_state(struct intel_display *display);
u32 intel_cmtg_sanitize_state(struct intel_display *display);

#endif /* __INTEL_CMTG_H__ */
