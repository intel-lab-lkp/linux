// SPDX-License-Identifier: GPL-2.0

#include <linux/xarray.h>

__rust_helper int rust_helper_xa_err(void *entry)
{
	return xa_err(entry);
}

__rust_helper void rust_helper_xa_init_flags(struct xarray *xa, gfp_t flags)
{
	return xa_init_flags(xa, flags);
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

__rust_helper int rust_helper_xa_reserve(struct xarray *xa, unsigned long index, gfp_t flags)
{
	return xa_reserve(xa, index, flags);
}

__rust_helper void rust_helper_xa_release(struct xarray *xa, unsigned long index)
{
	xa_release(xa, index);
}
