/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_DEBUGFS_PARAMS__
#define __I915_DEBUGFS_PARAMS__

struct dentry;
#define debugfs_node dentry
struct drm_i915_private;

struct debugfs_node *i915_debugfs_params(struct drm_i915_private *i915);

#endif /* __I915_DEBUGFS_PARAMS__ */
