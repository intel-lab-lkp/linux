/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __I915_DRM_CLIENT_H__
#define __I915_DRM_CLIENT_H__

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <uapi/drm/i915_drm.h>

#define I915_LAST_UABI_ENGINE_CLASS I915_ENGINE_CLASS_COMPUTE

struct drm_file;
struct drm_printer;

struct i915_drm_client {
	struct kref kref;

	unsigned int id;

	spinlock_t ctx_lock; /* For add/remove from ctx_list. */
	struct list_head ctx_list; /* List of contexts belonging to client. */

	/**
	 * @past_runtime: Accumulation of pphwsp runtimes from closed contexts.
	 */
	atomic64_t past_runtime[I915_LAST_UABI_ENGINE_CLASS + 1];

#ifdef CONFIG_CGROUP_DRM
	int throttle[I915_LAST_UABI_ENGINE_CLASS + 1];
	unsigned int over_budget;
	u64 last;
	u64 class_last[I915_LAST_UABI_ENGINE_CLASS + 1];
#endif
};

static inline struct i915_drm_client *
i915_drm_client_get(struct i915_drm_client *client)
{
	kref_get(&client->kref);
	return client;
}

void __i915_drm_client_free(struct kref *kref);

static inline void i915_drm_client_put(struct i915_drm_client *client)
{
	kref_put(&client->kref, __i915_drm_client_free);
}

struct i915_drm_client *i915_drm_client_alloc(void);

void i915_drm_client_fdinfo(struct drm_printer *p, struct drm_file *file);

u64 i915_drm_cgroup_get_active_time_us(struct drm_file *file);
int i915_drm_cgroup_signal_budget(struct drm_file *file,
				  u64 usage, u64 budget);

#endif /* !__I915_DRM_CLIENT_H__ */
