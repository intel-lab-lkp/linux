// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/kref.h>
#include <linux/ref_proxy.h>
#include <linux/slab.h>
#include <linux/srcu.h>

/**
 * struct ref_proxy_provider - A handle for resource provider.
 * @srcu: The SRCU to protect the resource.
 * @ref:  The pointer of resource.  It can point to anything.
 * @kref: The refcount for this handle.
 */
struct ref_proxy_provider {
	struct srcu_struct srcu;
	void __rcu *ref;
	struct kref kref;
};

/**
 * struct ref_proxy - A handle for resource consumer.
 * @rpp: The pointer of resource provider.
 * @idx: The index for the RCU critical section.
 */
struct ref_proxy {
	struct ref_proxy_provider *rpp;
	int idx;
};

/**
 * ref_proxy_provider_alloc() - Allocate struct ref_proxy_provider.
 * @ref: The pointer of resource.
 *
 * This holds an initial refcount to the struct.
 *
 * Return: The pointer of struct ref_proxy_provider.  NULL on errors.
 */
struct ref_proxy_provider *ref_proxy_provider_alloc(void *ref)
{
	struct ref_proxy_provider *rpp;

	rpp = kzalloc(sizeof(*rpp), GFP_KERNEL);
	if (!rpp)
		return NULL;

	init_srcu_struct(&rpp->srcu);
	rcu_assign_pointer(rpp->ref, ref);
	synchronize_srcu(&rpp->srcu);
	kref_init(&rpp->kref);

	return rpp;
}
EXPORT_SYMBOL(ref_proxy_provider_alloc);

static void ref_proxy_provider_release(struct kref *kref)
{
	struct ref_proxy_provider *rpp = container_of(kref,
			struct ref_proxy_provider, kref);

	cleanup_srcu_struct(&rpp->srcu);
	kfree(rpp);
}

/**
 * ref_proxy_provider_free() - Free struct ref_proxy_provider.
 * @rpp: The pointer of resource provider.
 *
 * This sets the resource `(struct ref_proxy_provider *)->ref` to NULL to
 * indicate the resource has gone.
 *
 * This drops the refcount to the resource provider.  If it is the final
 * reference, ref_proxy_provider_release() will be called to free the struct.
 */
void ref_proxy_provider_free(struct ref_proxy_provider *rpp)
{
	rcu_assign_pointer(rpp->ref, NULL);
	synchronize_srcu(&rpp->srcu);
	kref_put(&rpp->kref, ref_proxy_provider_release);
}
EXPORT_SYMBOL(ref_proxy_provider_free);

static void devm_ref_proxy_provider_free(void *data)
{
	struct ref_proxy_provider *rpp = data;

	ref_proxy_provider_free(rpp);
}

/**
 * devm_ref_proxy_provider_alloc() - Dev-managed ref_proxy_provider_alloc().
 * @dev: The device.
 * @ref: The pointer of resource.
 *
 * This holds an initial refcount to the struct.
 *
 * Return: The pointer of struct ref_proxy_provider.  NULL on errors.
 */
struct ref_proxy_provider *devm_ref_proxy_provider_alloc(struct device *dev,
							 void *ref)
{
	struct ref_proxy_provider *rpp;

	rpp = ref_proxy_provider_alloc(ref);
	if (rpp)
		if (devm_add_action_or_reset(dev, devm_ref_proxy_provider_free,
					     rpp))
			return NULL;

	return rpp;
}
EXPORT_SYMBOL(devm_ref_proxy_provider_alloc);

/**
 * ref_proxy_alloc() - Allocate struct ref_proxy_provider.
 * @rpp: The pointer of resource provider.
 *
 * This holds a refcount to the resource provider.
 *
 * Return: The pointer of struct ref_proxy_provider.  NULL on errors.
 */
struct ref_proxy *ref_proxy_alloc(struct ref_proxy_provider *rpp)
{
	struct ref_proxy *proxy;

	proxy = kzalloc(sizeof(*proxy), GFP_KERNEL);
	if (!proxy)
		return NULL;

	proxy->rpp = rpp;
	kref_get(&rpp->kref);

	return proxy;
}
EXPORT_SYMBOL(ref_proxy_alloc);

/**
 * ref_proxy_free() - Free struct ref_proxy.
 * @proxy: The pointer of struct ref_proxy.
 *
 * This drops a refcount to the resource provider.  If it is the final
 * reference, ref_proxy_provider_release() will be called to free the struct.
 */
void ref_proxy_free(struct ref_proxy *proxy)
{
	struct ref_proxy_provider *rpp = proxy->rpp;

	kref_put(&rpp->kref, ref_proxy_provider_release);
	kfree(proxy);
}
EXPORT_SYMBOL(ref_proxy_free);

/**
 * ref_proxy_get() - Get the resource.
 * @proxy: The pointer of struct ref_proxy.
 *
 * This tries to de-reference to the resource and enters a RCU critical
 * section.
 *
 * Return: The pointer to the resource.  NULL if the resource has gone.
 */
void __rcu *ref_proxy_get(struct ref_proxy *proxy)
{
	struct ref_proxy_provider *rpp = proxy->rpp;

	proxy->idx = srcu_read_lock(&rpp->srcu);
	return rcu_dereference(rpp->ref);
}
EXPORT_SYMBOL(ref_proxy_get);

/**
 * ref_proxy_put() - Put the resource.
 * @proxy: The pointer of struct ref_proxy.
 *
 * Call this function to indicate the resource is no longer used.  It exits
 * the RCU critical section.
 */
void ref_proxy_put(struct ref_proxy *proxy)
{
	struct ref_proxy_provider *rpp = proxy->rpp;

	srcu_read_unlock(&rpp->srcu, proxy->idx);
}
EXPORT_SYMBOL(ref_proxy_put);
