// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <uapi/drm/i915_drm.h>

#include <drm/drm_print.h>

#include "gem/i915_gem_context.h"
#include "i915_drm_client.h"
#include "i915_file_private.h"
#include "i915_gem.h"
#include "i915_utils.h"

struct i915_drm_client *i915_drm_client_alloc(void)
{
	struct i915_drm_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return NULL;

	kref_init(&client->kref);
	spin_lock_init(&client->ctx_lock);
	INIT_LIST_HEAD(&client->ctx_list);

	return client;
}

void __i915_drm_client_free(struct kref *kref)
{
	struct i915_drm_client *client =
		container_of(kref, typeof(*client), kref);

	kfree(client);
}

#if defined(CONFIG_PROC_FS) || defined(CONFIG_CGROUP_DRM)
static const char * const uabi_class_names[] = {
	[I915_ENGINE_CLASS_RENDER] = "render",
	[I915_ENGINE_CLASS_COPY] = "copy",
	[I915_ENGINE_CLASS_VIDEO] = "video",
	[I915_ENGINE_CLASS_VIDEO_ENHANCE] = "video-enhance",
	[I915_ENGINE_CLASS_COMPUTE] = "compute",
};

static u64 busy_add(struct i915_gem_context *ctx, unsigned int class)
{
	struct i915_gem_engines_iter it;
	struct intel_context *ce;
	u64 total = 0;

	for_each_gem_engine(ce, rcu_dereference(ctx->engines), it) {
		if (ce->engine->uabi_class != class)
			continue;

		total += intel_context_get_total_runtime_ns(ce);
	}

	return total;
}

static u64 get_class_active_ns(struct i915_drm_client *client,
			       struct drm_i915_private *i915,
			       unsigned int class,
			       unsigned int *capacity)
{
	struct i915_gem_context *ctx;
	u64 total;

	*capacity = i915->engine_uabi_class_count[class];
	if (!*capacity)
		return 0;

	total = atomic64_read(&client->past_runtime[class]);

	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &client->ctx_list, client_link)
		total += busy_add(ctx, class);
	rcu_read_unlock();

	return total;
}

static bool supports_stats(struct drm_i915_private *i915)
{
	return GRAPHICS_VER(i915) >= 8;
}
#endif

#if defined(CONFIG_CGROUP_DRM)
u64 i915_drm_cgroup_get_active_time_us(struct drm_file *file)
{
	struct drm_i915_file_private *fpriv = file->driver_priv;
	struct i915_drm_client *client = fpriv->client;
	struct drm_i915_private *i915 = fpriv->i915;
	unsigned int i;
	u64 busy = 0;

	if (!supports_stats(i915))
		return 0;

	for (i = 0; i < ARRAY_SIZE(uabi_class_names); i++) {
		unsigned int capacity;
		u64 b;

		b = get_class_active_ns(client, i915, i, &capacity);
		if (capacity) {
			b = DIV_ROUND_UP_ULL(b, capacity * 1000);
			busy += b;
		}
	}

	return busy;
}

int i915_drm_cgroup_signal_budget(struct drm_file *file, u64 usage, u64 budget)
{
	struct drm_i915_file_private *fpriv = file->driver_priv;
	u64 class_usage[I915_LAST_UABI_ENGINE_CLASS + 1];
	u64 class_last[I915_LAST_UABI_ENGINE_CLASS + 1];
	struct i915_drm_client *client = fpriv->client;
	struct drm_i915_private *i915 = fpriv->i915;
	struct intel_engine_cs *engine;
	bool over = usage > budget;
	struct task_struct *task;
	struct pid *pid;
	unsigned int i;
	ktime_t unused;
	int ret = 0;
	u64 t;

	if (!supports_stats(i915))
		return -EINVAL;

	if (usage == 0 && budget == 0)
		return 0;

	rcu_read_lock();
	pid = rcu_dereference(file->pid);
	task = pid_task(pid, PIDTYPE_TGID);
	if (over) {
		client->over_budget++;
		if (!client->over_budget)
			client->over_budget = 2;

		drm_dbg(&i915->drm, "%s[%u] over budget (%llu/%llu)\n",
			task ? task->comm : "<unknown>", pid_vnr(pid),
			usage, budget);
	} else {
		client->over_budget = 0;
		memset(client->class_last, 0, sizeof(client->class_last));
		memset(client->throttle, 0, sizeof(client->throttle));

		drm_dbg(&i915->drm, "%s[%u] un-throttled; under budget\n",
			task ? task->comm : "<unknown>", pid_vnr(pid));

		rcu_read_unlock();
		return 0;
	}
	rcu_read_unlock();

	memset(class_usage, 0, sizeof(class_usage));
	for_each_uabi_engine(engine, i915)
		class_usage[engine->uabi_class] +=
			ktime_to_ns(intel_engine_get_busy_time(engine, &unused));

	memcpy(class_last, client->class_last, sizeof(class_last));
	memcpy(client->class_last, class_usage, sizeof(class_last));

	for (i = 0; i < ARRAY_SIZE(uabi_class_names); i++)
		class_usage[i] -= class_last[i];

	t = client->last;
	client->last = ktime_get_raw_ns();
	t = client->last - t;

	if (client->over_budget == 1)
		return 0;

	for (i = 0; i < ARRAY_SIZE(uabi_class_names); i++) {
		u64 client_class_usage[I915_LAST_UABI_ENGINE_CLASS + 1];
		unsigned int capacity, rel_usage;

		if (!i915->engine_uabi_class_count[i])
			continue;

		t = DIV_ROUND_UP_ULL(t, 1000);
		class_usage[i] = DIV_ROUND_CLOSEST_ULL(class_usage[i], 1000);
		rel_usage = DIV_ROUND_CLOSEST_ULL(class_usage[i] * 100ULL,
						  t *
						  i915->engine_uabi_class_count[i]);
		if (rel_usage < 95) {
			/* Physical class not oversubsribed. */
			if (client->throttle[i]) {
				client->throttle[i] = 0;

				rcu_read_lock();
				pid = rcu_dereference(file->pid);
				task = pid_task(pid, PIDTYPE_TGID);
				drm_dbg(&i915->drm,
					"%s[%u] un-throttled; physical class %s utilisation %u%%\n",
					task ? task->comm : "<unknown>",
					pid_vnr(pid),
					uabi_class_names[i],
					rel_usage);
				rcu_read_unlock();
			}
			continue;
		}

		client_class_usage[i] =
			get_class_active_ns(client, i915, i, &capacity);
		if (client_class_usage[i]) {
			int permille;

			ret |= 1;

			permille = DIV_ROUND_CLOSEST_ULL((usage - budget) *
							 1000,
							 budget);
			client->throttle[i] =
			    DIV_ROUND_CLOSEST(permille *
					      I915_CONTEXT_MIN_USER_PRIORITY,
					      1000);
			if (client->throttle[i] <
			    I915_CONTEXT_MIN_USER_PRIORITY)
				client->throttle[i] =
					I915_CONTEXT_MIN_USER_PRIORITY;

			rcu_read_lock();
			pid = rcu_dereference(file->pid);
			task = pid_task(pid, PIDTYPE_TGID);
			drm_dbg(&i915->drm,
				"%s[%u] %d‰ over budget, throttled to priority %d; physical class %s utilisation %u%%\n",
				task ? task->comm : "<unknown>",
				pid_vnr(pid),
				permille,
				client->throttle[i],
				uabi_class_names[i],
				rel_usage);
			rcu_read_unlock();
		}
	}

	return ret;
}
#endif

#ifdef CONFIG_PROC_FS
static void
show_client_class(struct drm_printer *p,
		  struct drm_i915_private *i915,
		  struct i915_drm_client *client,
		  unsigned int class)
{
	unsigned int capacity;
	u64 total;

	total = get_class_active_ns(client, i915, class, &capacity);

	if (capacity)
		drm_printf(p, "drm-engine-%s:\t%llu ns\n",
			   uabi_class_names[class], total);

	if (capacity > 1)
		drm_printf(p, "drm-engine-capacity-%s:\t%u\n",
			   uabi_class_names[class],
			   capacity);
}

void i915_drm_client_fdinfo(struct drm_printer *p, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct drm_i915_private *i915 = file_priv->i915;
	unsigned int i;

	/*
	 * ******************************************************************
	 * For text output format description please see drm-usage-stats.rst!
	 * ******************************************************************
	 */

	if (!supports_stats(i915))
		return;

	for (i = 0; i < ARRAY_SIZE(uabi_class_names); i++)
		show_client_class(p, i915, file_priv->client, i);
}
#endif
