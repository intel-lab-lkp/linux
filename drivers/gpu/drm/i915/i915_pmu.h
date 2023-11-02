/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2017-2018 Intel Corporation
 */

#ifndef __I915_PMU_H__
#define __I915_PMU_H__

#include <uapi/drm/i915_drm.h>

struct drm_i915_private;
struct intel_gt;

#define I915_ENGINE_SAMPLE_COUNT (I915_SAMPLE_SEMA + 1)

struct i915_pmu_sample {
	u64 cur;
};

#ifdef CONFIG_PERF_EVENTS
int i915_pmu_init(void);
void i915_pmu_exit(void);
void i915_pmu_register(struct drm_i915_private *i915);
void i915_pmu_unregister(struct drm_i915_private *i915);
void i915_pmu_irq(struct drm_i915_private *i915);
void i915_pmu_gt_parked(struct intel_gt *gt);
void i915_pmu_gt_unparked(struct intel_gt *gt);
#else
static inline int i915_pmu_init(void) { return 0; }
static inline void i915_pmu_exit(void) {}
static inline void i915_pmu_register(struct drm_i915_private *i915) {}
static inline void i915_pmu_unregister(struct drm_i915_private *i915) {}
static inline void i915_pmu_irq(struct drm_i915_private *i915) {}
static inline void i915_pmu_gt_parked(struct intel_gt *gt) {}
static inline void i915_pmu_gt_unparked(struct intel_gt *gt) {}
#endif

#endif
