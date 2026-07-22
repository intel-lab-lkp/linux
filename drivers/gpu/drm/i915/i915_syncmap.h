/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2017 Intel Corporation
 */

#ifndef __I915_SYNCMAP_H__
#define __I915_SYNCMAP_H__

#include <linux/types.h>

struct i915_syncmap;
#define KSYNCMAP 16 /* radix of the tree, how many slots in each layer */

void i915_syncmap_init(struct i915_syncmap **root);
int i915_syncmap_set(struct i915_syncmap **root, u64 id, u32 seqno);
bool i915_syncmap_is_later(struct i915_syncmap **root, u64 id, u32 seqno);
void i915_syncmap_free(struct i915_syncmap **root);

#endif /* __I915_SYNCMAP_H__ */
