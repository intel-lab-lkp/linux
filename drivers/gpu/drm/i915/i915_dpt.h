/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Intel Corporation */

#ifndef __I915_DPT_H__
#define __I915_DPT_H__

#include <linux/types.h>

struct i915_address_space;
struct i915_vma;

struct i915_vma *intel_dpt_pin_to_ggtt(struct i915_address_space *vm,
				       unsigned int alignment);
void intel_dpt_unpin_from_ggtt(struct i915_address_space *vm);
u64 intel_dpt_offset(struct i915_vma *dpt_vma);

extern const struct intel_display_dpt_interface i915_display_dpt_interface;

#endif /* __I915_DPT_H__ */
