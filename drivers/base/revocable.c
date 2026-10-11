// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Google LLC
 *
 * Revocable resource management
 */

#include <linux/kref.h>
#include <linux/revocable.h>
#include <linux/slab.h>
#include <linux/srcu.h>

/**
 * DOC: Overview
 *
 * The "revocable" mechanism is a synchronization primitive designed to
 * manage safe access to resources that can be asynchronously removed or
 * invalidated.  Its primary purpose is to prevent Use-After-Free (UAF)
 * errors when interacting with resources whose lifetimes are not
 * guaranteed to outlast their consumers.
 *
 * This is particularly useful in systems where resources can disappear
 * unexpectedly, such as those provided by hot-pluggable devices like
 * USB.  When a consumer holds a reference to such a resource, the
 * underlying device might be removed, causing the resource's memory to
 * be freed.  Subsequent access attempts by the consumer would then lead
 * to UAF errors.
 *
 * Revocable addresses this by providing a form of "weak reference" and
 * a controlled access method.  It allows a resource consumer to safely
 * attempt to access the resource.  The mechanism guarantees that any
 * access granted is valid for the duration of its use.  If the resource
 * has already been revoked (i.e., freed), the access attempt will fail
 * safely, typically by returning NULL, instead of causing a crash.
 *
 * It uses a provider/consumer model built on Sleepable RCU (SRCU) to
 * guarantee safe memory access:
 *
 * - A resource provider, such as a driver for a hot-pluggable device,
 *   initializes a struct revocable with a pointer to the resource.
 *   The provider handle can either be embedded in another structure
 *   or dynamically allocated on the heap.
 *
 * - A resource consumer that wants to access the resource allocates a
 *   struct revocable_handle containing a reference to the provider.
 *
 * - To access the resource, the consumer uses revocable_try_access().
 *   This function enters an SRCU read-side critical section and returns
 *   the pointer to the resource.  If the provider has already freed the
 *   resource, it returns NULL.  After use, the consumer calls
 *   revocable_withdraw_access() to exit the SRCU critical section.  There
 *   are some macro level helpers for doing that.
 *
 *   The API provides the following contract:
 *
 *   - revocable_try_access() can be safely called from both process and
 *     atomic contexts.
 *   - It is permitted to sleep within the critical section established
 *     between revocable_try_access() and revocable_withdraw_access().
 *   - revocable_try_access() and the matching revocable_withdraw_access()
 *     must occur in the same context.  For example, it is illegal to
 *     invoke revocable_withdraw_access() in an irq handler if the matching
 *     revocable_try_access() was invoked in process context.
 *
 * - When the provider needs to remove the resource, it calls
 *   revocable_revoke().  This function sets the internal resource
 *   pointer to NULL and then calls synchronize_srcu() to wait for all
 *   current readers to finish before the resource can be completely torn
 *   down.
 */

/**
 * revocable_init() - Initialize struct revocable.
 * @rev: The pointer of resource provider.
 * @res: The pointer of resource.
 *
 * This initializes a resource provider handle embedded within another struct.
 */
int revocable_init(struct revocable *rev, void *res)
{
	int ret;

	ret = init_srcu_struct(&rev->srcu);
	if (ret)
		return ret;

	rev->dynamic = false;
	RCU_INIT_POINTER(rev->res, res);
	return 0;
}
EXPORT_SYMBOL_GPL(revocable_init);

/**
 * revocable_destroy() - Destroy struct revocable.
 * @rev: The pointer of resource provider.
 *
 * This destroys a resource provider handle embedded within another struct.
 */
void revocable_destroy(struct revocable *rev)
{
	cleanup_srcu_struct(&rev->srcu);
}
EXPORT_SYMBOL_GPL(revocable_destroy);

/**
 * revocable_alloc() - Allocate struct revocable.
 * @res: The pointer of resource.
 *
 * This allocates a resource provider handle with an initial reference count
 * of 1.  On success, the caller owns the initial reference and must call
 * revocable_put() to drop it when done.
 *
 * Additional references can be acquired and released with revocable_get()
 * and revocable_put().
 *
 * Return: The pointer of struct revocable.  NULL on errors.
 */
struct revocable *revocable_alloc(void *res)
{
	struct revocable *rev;
	int ret;

	rev = kzalloc_obj(*rev);
	if (!rev)
		return NULL;

	ret = revocable_init(rev, res);
	if (ret) {
		kfree(rev);
		return NULL;
	}

	rev->dynamic = true;
	kref_init(&rev->kref);
	return rev;
}
EXPORT_SYMBOL_GPL(revocable_alloc);

/**
 * revocable_get() - Increment the reference count of the provider handle.
 * @rev: The pointer of resource provider.
 *
 * (Only for dynamically allocated provider handles)
 * This increments the reference count of @rev.
 */
void revocable_get(struct revocable *rev)
{
	if (!rev->dynamic)
		return;
	kref_get(&rev->kref);
}
EXPORT_SYMBOL_GPL(revocable_get);

static void revocable_release_worker(struct work_struct *work)
{
	struct revocable *rev = container_of(work, typeof(*rev), release_work);

	revocable_destroy(rev);
	kfree(rev);
}

static void revocable_release(struct kref *kref)
{
	struct revocable *rev = container_of(kref, typeof(*rev), kref);

	/*
	 * Defer cleanup to a process context safely, because the final
	 * kref_put() might happen in an atomic context which is unsafe for
	 * cleanup_srcu_struct().
	 */
	INIT_WORK(&rev->release_work, revocable_release_worker);
	schedule_work(&rev->release_work);
}

/**
 * revocable_put() - Decrement the reference count of the provider handle.
 * @rev: The pointer of resource provider.
 *
 * (Only for dynamically allocated provider handles)
 * This drops a reference to the resource provider.  If it is the final
 * reference, revocable_release() will be called to free the handle and its
 * internal resources.
 */
void revocable_put(struct revocable *rev)
{
	if (!rev->dynamic)
		return;
	kref_put(&rev->kref, revocable_release);
}
EXPORT_SYMBOL_GPL(revocable_put);

/**
 * revocable_revoke() - Revoke the managed resource.
 * @rev: The pointer of resource provider.
 *
 * This invalidates the resource by setting `(struct revocable *)->res` to NULL
 * to indicate the resource has gone.
 */
void revocable_revoke(struct revocable *rev)
{
	rcu_assign_pointer(rev->res, NULL);
	synchronize_srcu(&rev->srcu);
}
EXPORT_SYMBOL_GPL(revocable_revoke);

/**
 * revocable_handle_init() - Initialize struct revocable_handle.
 * @rev: The pointer of resource provider.
 * @rh: The pointer of resource_handle.
 *
 * This initializes a handle owned by the consumer.
 *
 * (Only for dynamically allocated provider handles)
 * This holds a reference to the resource provider.
 */
void revocable_handle_init(struct revocable *rev, struct revocable_handle *rh)
{
	rh->rev = rev;
	/*
	 * Cache @rev->dynamic so revocable_handle_destroy() does not need to
	 * dereference @rh->rev, which may already be freed if @rev is embedded.
	 */
	rh->dynamic = rev->dynamic;

	if (!rh->dynamic)
		return;

	kref_get(&rev->kref);
}
EXPORT_SYMBOL_GPL(revocable_handle_init);

/**
 * revocable_handle_destroy() - Destroy struct revocable_handle.
 * @rh: The pointer of resource_handle.
 *
 * (Only for dynamically allocated provider handles)
 * This drops a reference to the resource provider.  If it is the final
 * reference, revocable_release() will be called to free the handle and its
 * internal resources.
 */
void revocable_handle_destroy(struct revocable_handle *rh)
{
	/*
	 * For an embedded @rh->rev, no kref is held, so the provider may free
	 * @rh->rev as soon as revocable_withdraw_access() exits the SRCU
	 * critical section.  Check the cached @rh->dynamic flag to avoid
	 * dereferencing a potentially freed @rh->rev.
	 */
	if (!rh->dynamic)
		return;

	kref_put(&rh->rev->kref, revocable_release);
}
EXPORT_SYMBOL_GPL(revocable_handle_destroy);

/**
 * revocable_try_access() - Try to access the resource.
 * @rh: The pointer of resource_handle.
 *
 * This tries to de-reference to the resource and enters a SRCU critical
 * section.
 *
 * The function is safe to be called from both process and atomic contexts.
 * While holding the access (i.e. before calling revocable_withdraw_access()),
 * the caller is allowed to sleep.
 *
 * Note that revocable_try_access() and the matching
 * revocable_withdraw_access() must occur in the same context.  For example, it
 * is illegal to invoke revocable_withdraw_access() in an irq handler if the
 * matching revocable_try_access() was invoked in process context.
 *
 * Return: The pointer to the resource.  NULL if the resource has gone.
 */
void *revocable_try_access(struct revocable_handle *rh)
	__acquires(&rh->rev->srcu)
{
	struct revocable *rev = rh->rev;

	rh->idx = srcu_read_lock(&rev->srcu);
	return srcu_dereference(rev->res, &rev->srcu);
}
EXPORT_SYMBOL_GPL(revocable_try_access);

/**
 * revocable_withdraw_access() - Stop accessing to the resource.
 * @rh: The pointer of resource_handle.
 *
 * Call this function to indicate the resource is no longer used.  It exits
 * the SRCU critical section.
 *
 * The function is safe to be called from both process and atomic contexts.
 *
 * Note that revocable_try_access() and the matching
 * revocable_withdraw_access() must occur in the same context.  For example, it
 * is illegal to invoke revocable_withdraw_access() in an irq handler if the
 * matching revocable_try_access() was invoked in process context.
 */
void revocable_withdraw_access(struct revocable_handle *rh)
	__releases(&rh->rev->srcu)
{
	struct revocable *rev = rh->rev;

	srcu_read_unlock(&rev->srcu, rh->idx);
}
EXPORT_SYMBOL_GPL(revocable_withdraw_access);
