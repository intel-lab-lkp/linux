// SPDX-License-Identifier: GPL-2.0

#include <linux/xarray.h>

__rust_helper int rust_helper_xa_err(void *entry)
{
	return xa_err(entry);
}

/*
 * `xa_init_flags()` expands `spin_lock_init()`, which generates one static
 * `lock_class_key` per compilation unit. Going through a single Rust binding
 * helper would make every Rust `XArray` instance share that one key, causing
 * false-positive lock-ordering reports from lockdep. Re-register the
 * spinlock's lockdep class with a key supplied by the caller so each Rust
 * instantiation site can have its own.
 */
__rust_helper void rust_helper_xa_init_flags_with_key(struct xarray *xa,
						      gfp_t flags,
						      const char *name,
						      struct lock_class_key *key)
{
	xa_init_flags(xa, flags);
	lockdep_set_class_and_name(&xa->xa_lock, key, name);
}

__rust_helper int rust_helper_xa_trylock(struct xarray *xa)
{
	return xa_trylock(xa);
}

__rust_helper void rust_helper_xa_lock(struct xarray *xa)
{
	return xa_lock(xa);
}

__rust_helper void rust_helper_xa_unlock(struct xarray *xa)
{
	return xa_unlock(xa);
}
