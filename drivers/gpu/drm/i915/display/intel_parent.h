/* SPDX-License-Identifier: MIT */
/* Copyright © 2025 Intel Corporation */

#ifndef __INTEL_PARENT_H__
#define __INTEL_PARENT_H__

#include <linux/types.h>

struct dma_fence;
struct intel_display;

bool intel_parent_irq_enabled(struct intel_display *display);
void intel_parent_irq_synchronize(struct intel_display *display);

bool intel_parent_rps_available(struct intel_display *display);
void intel_parent_rps_boost(struct intel_display *display, struct dma_fence *fence);
void intel_parent_rps_mark_interactive(struct intel_display *display, bool interactive);
void intel_parent_rps_ilk_irq_handler(struct intel_display *display);

bool intel_parent_vgpu_active(struct intel_display *display);

bool intel_parent_fence_support_legacy(struct intel_display *display);

#endif /* __INTEL_PARENT_H__ */
