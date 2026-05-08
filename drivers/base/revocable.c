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
 *   allocates a struct revocable and initializes it with a pointer
 *   to the resource.
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

static void revocable_release(struct kref *kref)
{
	struct revocable *rev = container_of(kref, typeof(*rev), kref);

	cleanup_srcu_struct(&rev->srcu);

	if (!rev->embedded)
		kfree(rev);
}

/**
 * revocable_alloc() - Allocate struct revocable.
 * @res: The pointer of resource.
 *
 * This allocates a resource provider handle and holds 2 initial reference
 * counts to the handle.  If revocable_alloc() succeed:
 *
 * - The provider should call revocable_revoke() for dropping a reference.
 * - The caller should call revocable_put() for dropping another reference.
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

	rev->embedded = false;
	return rev;
}
EXPORT_SYMBOL_GPL(revocable_alloc);

/**
 * revocable_init() - Initialize struct revocable.
 * @rev: The pointer of resource provider.
 * @res: The pointer of resource.
 *
 * This initializes a resource provider handle embedded within another
 * structure and holds 2 initial reference counts to the handle.
 *
 * If revocable_init() succeed:
 *
 * - The provider should call revocable_revoke() for dropping a reference.
 * - The caller should call revocable_put() for dropping another reference.
 */
int revocable_init(struct revocable *rev, void *res)
{
	int ret;

	ret = init_srcu_struct(&rev->srcu);
	if (ret)
		return ret;

	RCU_INIT_POINTER(rev->res, res);
	kref_init(&rev->kref);
	kref_get(&rev->kref);
	rev->embedded = true;
	return 0;
}
EXPORT_SYMBOL_GPL(revocable_init);

/**
 * revocable_revoke() - Revoke the managed resource.
 * @rev: The pointer of resource provider.
 *
 * This sets the resource `(struct revocable *)->res` to NULL to indicate
 * the resource has gone.
 *
 * This drops a reference count to the resource provider.  If it is the
 * final reference, revocable_release() will be called to free the internal
 * resources.
 */
void revocable_revoke(struct revocable *rev)
{
	rcu_assign_pointer(rev->res, NULL);
	synchronize_srcu(&rev->srcu);
	revocable_put(rev);
}
EXPORT_SYMBOL_GPL(revocable_revoke);

/**
 * revocable_get() - Increase a reference count to the provider handle.
 * @rev: The pointer of resource provider.
 *
 * This increments the reference count.
 */
void revocable_get(struct revocable *rev)
{
	kref_get(&rev->kref);
}
EXPORT_SYMBOL_GPL(revocable_get);

/**
 * revocable_put() - Decrease a reference count to the provider handle.
 * @rev: The pointer of resource provider.
 *
 * This drops a reference count to the resource provider.  If it is the
 * final reference, revocable_release() will be called to free the internal
 * resources.
 */
void revocable_put(struct revocable *rev)
{
	kref_put(&rev->kref, revocable_release);
}
EXPORT_SYMBOL_GPL(revocable_put);

/**
 * revocable_handle_init() - Initialize struct revocable_handle.
 * @rev: The pointer of resource provider.
 * @rh: The pointer of resource_handle.
 *
 * This initializes a handle owned by the consumer and holds a reference
 * count to the resource provider.
 */
void revocable_handle_init(struct revocable *rev, struct revocable_handle *rh)
{
	revocable_get(rev);
	rh->rev = rev;
}
EXPORT_SYMBOL_GPL(revocable_handle_init);

/**
 * revocable_handle_deinit() - Deinitialize struct revocable_handle.
 * @rh: The pointer of resource_handle.
 *
 * This drops a reference count to the resource provider.  If it is the
 * final reference, revocable_release() will be called to free the internal
 * resources.
 */
void revocable_handle_deinit(struct revocable_handle *rh)
{
	struct revocable *rev = rh->rev;

	revocable_put(rev);
}
EXPORT_SYMBOL_GPL(revocable_handle_deinit);

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
