/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef __I915_CONFIG_H__
#define __I915_CONFIG_H__

#include <linux/types.h>

unsigned long i915_fence_context_timeout(u64 context);
unsigned long i915_fence_timeout(void);

#endif /* __I915_CONFIG_H__ */
