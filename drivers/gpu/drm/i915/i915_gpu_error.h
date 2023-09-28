/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2008-2018 Intel Corporation
 */

#ifndef _I915_GPU_ERROR_H_
#define _I915_GPU_ERROR_H_

#include <linux/atomic.h>
#include <linux/types.h>

#include "gt/intel_engine_types.h"

struct drm_i915_error_state_buf;
struct drm_i915_private;
struct i915_gpu_coredump;

struct i915_gpu_error {
	/* For reset and error_state handling. */
	spinlock_t lock;
	/* Protected by the above dev->gpu_error.lock. */
	struct i915_gpu_coredump *first_error;

	atomic_t pending_fb_pin;

	/** Number of times the device has been reset (global) */
	atomic_t reset_count;

	/** Number of times an engine has been reset */
	atomic_t reset_engine_count[I915_NUM_ENGINES];
};

static inline u32 i915_reset_count(struct i915_gpu_error *error)
{
	return atomic_read(&error->reset_count);
}

static inline u32 i915_reset_engine_count(struct i915_gpu_error *error,
					  const struct intel_engine_cs *engine)
{
	return atomic_read(&error->reset_engine_count[engine->uabi_class]);
}

#define CORE_DUMP_FLAG_NONE           0x0
#define CORE_DUMP_FLAG_IS_GUC_CAPTURE BIT(0)

#if IS_ENABLED(CONFIG_DRM_I915_CAPTURE_ERROR) && IS_ENABLED(CONFIG_DRM_I915_DEBUG_GEM)
void intel_klog_error_capture(struct intel_gt *gt,
			      intel_engine_mask_t engine_mask);
#else
static inline void intel_klog_error_capture(struct intel_gt *gt,
					    intel_engine_mask_t engine_mask)
{
}
#endif

#if IS_ENABLED(CONFIG_DRM_I915_CAPTURE_ERROR)

__printf(2, 3)
void i915_error_printf(struct drm_i915_error_state_buf *e, const char *f, ...);

void i915_capture_error_state(struct intel_gt *gt,
			      intel_engine_mask_t engine_mask, u32 dump_flags);

struct i915_gpu_coredump *
i915_gpu_coredump_alloc(struct intel_engine_cs *engine, gfp_t gfp);

void i915_gpu_error_execlist_capture(struct i915_gpu_coredump *error,
				     struct i915_request *rq);

ssize_t
i915_gpu_coredump_copy_to_buffer(struct i915_gpu_coredump *error,
				 char *buf, loff_t offset, size_t count);

void i915_gpu_coredump_put(struct i915_gpu_coredump *gpu);

void i915_reset_error_state(struct drm_i915_private *i915);
void i915_disable_error_state(struct drm_i915_private *i915, int err);

void i915_gpu_error_debugfs_register(struct drm_i915_private *i915);
void i915_gpu_error_sysfs_setup(struct drm_i915_private *i915);
void i915_gpu_error_sysfs_teardown(struct drm_i915_private *i915);

#else

__printf(2, 3)
static inline void
i915_error_printf(struct drm_i915_error_state_buf *e, const char *f, ...)
{
}

static inline void
i915_capture_error_state(struct intel_gt *gt, intel_engine_mask_t engine_mask, u32 dump_flags)
{
}

static inline struct i915_gpu_coredump *
i915_gpu_coredump_alloc(struct drm_i915_private *i915, gfp_t gfp)
{
	return NULL;
}

static inline void i915_gpu_error_execlist_capture(struct i915_gpu_coredump *error,
						   struct i915_request *rq)
{
}

static inline void i915_gpu_coredump_put(struct i915_gpu_coredump *gpu)
{
}

static inline void i915_reset_error_state(struct drm_i915_private *i915)
{
}

static inline void i915_disable_error_state(struct drm_i915_private *i915,
					    int err)
{
}

static inline void i915_gpu_error_debugfs_register(struct drm_i915_private *i915)
{
}

static inline void i915_gpu_error_sysfs_setup(struct drm_i915_private *i915)
{
}

static inline void i915_gpu_error_sysfs_teardown(struct drm_i915_private *i915)
{
}

#endif /* IS_ENABLED(CONFIG_DRM_I915_CAPTURE_ERROR) */

#endif /* _I915_GPU_ERROR_H_ */
