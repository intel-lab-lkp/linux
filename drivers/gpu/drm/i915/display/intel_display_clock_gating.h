/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 Intel Corporation
 */

#ifndef __INTEL_DISPLAY_CLOCK_GATING_H__
#define __INTEL_DISPLAY_CLOCK_GATING_H__

struct intel_display;

void intel_display_init_clock_gating_early(struct intel_display *display);
void intel_display_init_clock_gating_late(struct intel_display *display);

#endif /* __INTEL_DISPLAY_CLOCK_GATING_H__ */
