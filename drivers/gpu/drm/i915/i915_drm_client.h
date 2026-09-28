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

#include "i915_file_private.h"
#include "gem/i915_gem_object_types.h"
#include "gt/intel_context_types.h"

#define I915_LAST_UABI_ENGINE_CLASS I915_ENGINE_CLASS_COMPUTE

struct drm_file;
struct drm_printer;

struct i915_drm_client {
	struct kref kref;

	spinlock_t ctx_lock; /* For add/remove from ctx_list. */
	struct list_head ctx_list; /* List of contexts belonging to client. */

#ifdef CONFIG_PROC_FS
	/**
	 * @objects_lock: lock protecting @objects_list
	 */
	spinlock_t objects_lock;

	/**
	 * @objects_list: list of objects created by this client
	 *
	 * Protected by @objects_lock.
	 */
	struct list_head objects_list;
#endif

	/**
	 * @past_runtime: Accumulation of pphwsp runtimes from closed contexts.
	 */
	atomic64_t past_runtime[I915_LAST_UABI_ENGINE_CLASS + 1];

	/**
	 * @wa_22013059131_owner: Who programs Wa_22013059131 for this client.
	 *
	 * One of the I915_WA_22013059131_* values below.  Latched once, on
	 * the first of either I915_CONTEXT_PARAM_WA_22013059131 being set or
	 * a context of this client initialising its LRC, and read-only
	 * afterwards.  See i915_drm_client_latch_wa_22013059131().
	 */
	atomic_t wa_22013059131_owner;
};

/*
 * Wa_22013059131:dg2 - who programs LSC_CHICKEN_BIT_0's
 * FORCE_1_SUB_MESSAGE_PER_FRAGMENT bit for the contexts of a client.
 *
 * LSC_CHICKEN_BIT_0 is shared by the RCS and CCS engines rather than being
 * part of the saved context image, so it cannot be tracked per context.
 * GuC only lets RCS and CCS run concurrently when their contexts share an
 * address space, so tracking this per DRM client (which forces every
 * address space of that client to agree) is sufficient to avoid contexts
 * that could actually run at the same time racing over the register.
 *
 * I915_WA_22013059131_UNSET must be 0 as the client is zero-initialised.
 */
#define I915_WA_22013059131_UNSET	0
#define I915_WA_22013059131_KERNEL	1
#define I915_WA_22013059131_USERSPACE	2

/**
 * i915_drm_client_latch_wa_22013059131 - Latch Wa_22013059131 ownership
 * @client: the DRM client
 * @want: I915_WA_22013059131_KERNEL or I915_WA_22013059131_USERSPACE
 *
 * Claim @want as the owner of Wa_22013059131 for @client, unless an owner
 * has already been latched.  Callers that must not change the owner pass
 * the value they rely on and compare it against the return value.
 *
 * The value is latched with a single cmpxchg so that no extra locking is
 * needed.  This matters because the callers run with different locks held:
 * GEM_CONTEXT_CREATE_EXT_SETPARAM does not hold &proto_context_lock, while
 * GEM_CONTEXT_SETPARAM does, and the LRC init path holds neither.
 *
 * Returns: the owner in effect for @client, never I915_WA_22013059131_UNSET.
 */
static inline int
i915_drm_client_latch_wa_22013059131(struct i915_drm_client *client, int want)
{
	int owner;

	owner = atomic_cmpxchg(&client->wa_22013059131_owner,
			       I915_WA_22013059131_UNSET, want);

	return owner == I915_WA_22013059131_UNSET ? want : owner;
}

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

#ifdef CONFIG_PROC_FS
void i915_drm_client_add_object(struct i915_drm_client *client,
				struct drm_i915_gem_object *obj);
void i915_drm_client_remove_object(struct drm_i915_gem_object *obj);
void i915_drm_client_add_context_objects(struct i915_drm_client *client,
					 struct intel_context *ce);
#else
static inline void i915_drm_client_add_object(struct i915_drm_client *client,
					      struct drm_i915_gem_object *obj)
{
}

static inline void
i915_drm_client_remove_object(struct drm_i915_gem_object *obj)
{
}

static inline void
i915_drm_client_add_context_objects(struct i915_drm_client *client,
				    struct intel_context *ce)
{
}
#endif

#endif /* !__I915_DRM_CLIENT_H__ */
