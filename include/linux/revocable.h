/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2026 Google LLC
 */

#ifndef __LINUX_REVOCABLE_H
#define __LINUX_REVOCABLE_H

#include <linux/cleanup.h>
#include <linux/kref.h>
#include <linux/srcu.h>

/**
 * struct revocable - A handle for resource provider.
 * @srcu: The SRCU to protect the resource.
 * @res:  The pointer of resource.  It can point to anything.
 * @kref: The refcount for this handle.
 * @embedded: Indicate if the handle is embedded in another struct.
 *
 * Note: All members of this structure are intended to be opaque and should
 * not be accessed directly by the users.
 */
struct revocable {
	struct srcu_struct srcu;
	void __rcu *res;
	struct kref kref;
	bool embedded;
};

/**
 * struct revocable_handle - A handle for resource consumer.
 * @rev: The pointer of resource provider.
 * @idx: The index for the SRCU critical section.
 *
 * Note: All members of this structure are intended to be opaque and should
 * not be accessed directly by the users.
 */
struct revocable_handle {
	struct revocable *rev;
	int idx;
};

struct revocable *revocable_alloc(void *res);
int revocable_init(struct revocable *rev, void *res);
void revocable_revoke(struct revocable *rev);
void revocable_get(struct revocable *rev);
void revocable_put(struct revocable *rev);

void revocable_handle_init(struct revocable *rev, struct revocable_handle *rh);
void revocable_handle_deinit(struct revocable_handle *rh);
void *revocable_try_access(struct revocable_handle *rh)
	__acquires(&rh->rev->srcu);
void revocable_withdraw_access(struct revocable_handle *rh)
	__releases(&rh->rev->srcu);

DEFINE_FREE(access_rev, struct revocable_handle *, {
	revocable_withdraw_access(_T);
	revocable_handle_deinit(_T);
})

#define _revocable_try_access_with(_rev, _rh, _res)				\
	struct revocable_handle _rh;						\
	struct revocable_handle *__UNIQUE_ID(name) __free(access_rev) = &_rh;	\
										\
	revocable_handle_init(_rev, &_rh);					\
	_res = revocable_try_access(&_rh)

/**
 * revocable_try_access_with() - A helper for accessing revocable resource
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * The macro simplifies the access-release cycle for consumers, ensuring that
 * corresponding revocable_withdraw_access() and revocable_handle_deinit() are
 * called, even in the case of an early exit.
 *
 * It creates a local variable in the current scope.  @_res is populated with
 * the result of revocable_try_access().  Callers **must** check if @_res is
 * ``NULL`` before using it.  The revocable_withdraw_access() function is
 * automatically called when the scope is exited.
 *
 * Note: It shares the same issue with guard() in cleanup.h.  No goto statements
 * are allowed before the helper.  Otherwise, the compiler fails with
 * "jump bypasses initialization of variable with __attribute__((cleanup))".
 */
#define revocable_try_access_with(_rev, _res)					\
	_revocable_try_access_with(_rev, __UNIQUE_ID(name), _res)

/**
 * revocable_try_access_or_return_err() - Variant of revocable_try_access_with()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 * @_err: The error code to return if resource is revoked.
 *
 * Similar to revocable_try_access_with() but returns from the current function
 * with @_err if the resource is revoked.  Callers don't need to check @_res for
 * ``NULL`` as this handles the revocation case by returning early.
 */
#define revocable_try_access_or_return_err(_rev, _res, _err)			\
	_revocable_try_access_with(_rev, __UNIQUE_ID(name), _res);		\
	if (!_res)								\
		return _err

/**
 * revocable_try_access_or_return() - Variant of revocable_try_access_with()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * Similar to revocable_try_access_or_return_err() but returns -ENODEV if the
 * resource is revoked.
 */
#define revocable_try_access_or_return(_rev, _res)				\
	revocable_try_access_or_return_err(_rev, _res, -ENODEV)

/**
 * revocable_try_access_or_return_void() - Variant of revocable_try_access_with()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * Similar to revocable_try_access_or_return_err() but returns void if the
 * resource is revoked.
 */
#define revocable_try_access_or_return_void(_rev, _res)				\
	revocable_try_access_or_return_err(_rev, _res, )

#define _revocable_try_access_with_scoped(_rev, _rh, _label, _res)		\
	for (struct revocable_handle _rh,					\
			*__UNIQUE_ID(name) __free(access_rev) = &_rh;		\
	     ({ revocable_handle_init(_rev, &_rh);				\
		_res = revocable_try_access(&_rh);				\
		true; });							\
	     ({ goto _label; }))						\
		if (0) {							\
_label:										\
			break;							\
		} else

/**
 * revocable_try_access_with_scoped() - Variant of revocable_try_access_with()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * Similar to revocable_try_access_with() but with an explicit scope from a
 * temporary ``for`` loop.
 */
#define revocable_try_access_with_scoped(_rev, _res)				\
	_revocable_try_access_with_scoped(_rev, __UNIQUE_ID(name),		\
					  __UNIQUE_ID(label), _res)

/**
 * revocable_try_access_or_return_err_scoped() - Variant of revocable_try_access_with_scoped()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 * @_err: The error code to return if resource is revoked.
 *
 * Similar to revocable_try_access_with_scoped() but returns from the current
 * function with @_err if the resource is revoked.  Callers don't need to check
 * @_res for ``NULL`` as this handles the revocation case by returning early.
 */
#define revocable_try_access_or_return_err_scoped(_rev, _res, _err)		\
	_revocable_try_access_with_scoped(_rev, __UNIQUE_ID(name),		\
					  __UNIQUE_ID(label), _res)		\
	if (!_res) {								\
		return _err;							\
	} else

/**
 * revocable_try_access_or_return_scoped() - Variant of revocable_try_access_with_scoped()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * Similar to revocable_try_access_or_return_err_scoped() but returns -ENODEV
 * if the resource is revoked.
 */
#define revocable_try_access_or_return_scoped(_rev, _res)			\
	revocable_try_access_or_return_err_scoped(_rev, _res, -ENODEV)

/**
 * revocable_try_access_or_return_void_scoped() - Variant of revocable_try_access_with_scoped()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * Similar to revocable_try_access_or_return_err_scoped() but returns void
 * if the resource is revoked.
 */
#define revocable_try_access_or_return_void_scoped(_rev, _res)			\
	revocable_try_access_or_return_err_scoped(_rev, _res, )

/**
 * revocable_try_access_or_skip_scoped() - Variant of revocable_try_access_with_scoped()
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * Similar to revocable_try_access_with_scoped() but skips the following code
 * block if the resource is revoked.
 */
#define revocable_try_access_or_skip_scoped(_rev, _res)				\
	_revocable_try_access_with_scoped(_rev, __UNIQUE_ID(name),		\
					  __UNIQUE_ID(label), _res)		\
	if (!_res) {								\
		break;								\
	} else

#endif /* __LINUX_REVOCABLE_H */
