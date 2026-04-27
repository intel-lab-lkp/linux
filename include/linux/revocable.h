/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2026 Google LLC
 */

#ifndef __LINUX_REVOCABLE_H
#define __LINUX_REVOCABLE_H

#include <linux/cleanup.h>
#include <linux/compiler.h>
#include <linux/kref.h>
#include <linux/srcu.h>

/**
 * enum revocable_alloc_type - The allocation method for a revocable provider.
 * @REVOCABLE_DYNAMIC: The struct revocable was dynamically allocated using
 *                     revocable_alloc() and its lifetime is managed by
 *                     reference counting.
 * @REVOCABLE_EMBEDDED: The struct revocable is embedded within another
 *                      structure.  Its lifetime is tied to the parent
 *                      structure and is not reference counted.
 */
enum revocable_alloc_type {
	REVOCABLE_DYNAMIC,
	REVOCABLE_EMBEDDED,
};

/**
 * struct revocable - A handle for resource provider.
 * @srcu: The SRCU to protect the resource.
 * @res:  The pointer of resource.  It can point to anything.
 * @kref: The refcount for this handle.
 * @alloc_type: The memory allocation type.
 */
struct revocable {
	struct srcu_struct srcu;
	void __rcu *res;
	struct kref kref;
	enum revocable_alloc_type alloc_type;
};

/**
 * struct revocable_consumer - A handle for resource consumer.
 * @rev: The pointer of resource provider.
 * @idx: The index for the SRCU critical section.
 */
struct revocable_consumer {
	struct revocable *rev;
	int idx;
};

void revocable_get(struct revocable *rev);
void revocable_put(struct revocable *rev);

struct revocable *revocable_alloc(void *res);
void revocable_revoke(struct revocable *rev);
int revocable_embed_init(struct revocable *rev, void *res);
void revocable_embed_destroy(struct revocable *rev);

void revocable_init(struct revocable *rev, struct revocable_consumer *rc);
void revocable_deinit(struct revocable_consumer *rc);
void *revocable_try_access(struct revocable_consumer *rc)
	__acquires(&rc->rev->srcu);
void revocable_withdraw_access(struct revocable_consumer *rc)
	__releases(&rc->rev->srcu);

DEFINE_FREE(access_rev, struct revocable_consumer *, {
	revocable_withdraw_access(_T);
	revocable_deinit(_T);
})

#define _revocable_try_access_with(_rev, _rc, _res)				\
	struct revocable_consumer _rc;						\
	struct revocable_consumer *__UNIQUE_ID(name) __free(access_rev) = &_rc;	\
	revocable_init(_rev, &_rc);						\
	_res = revocable_try_access(&_rc)

/**
 * revocable_try_access_with() - A helper for accessing revocable resource
 * @_rev: The pointer of resource provider.
 * @_res: A pointer variable that will be assigned the resource.
 *
 * The macro simplifies the access-release cycle for consumers, ensuring that
 * corresponding revocable_withdraw_access() and revocable_deinit() are called,
 * even in the case of an early exit.
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

#define _revocable_try_access_with_scoped(_rev, _rc, _label, _res)		\
	for (struct revocable_consumer _rc,					\
			*__UNIQUE_ID(name) __free(access_rev) = &_rc;		\
	     ({ revocable_init(_rev, &_rc);					\
		_res = revocable_try_access(&_rc);				\
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
