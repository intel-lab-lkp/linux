// SPDX-License-Identifier: GPL-2.0

#include <linux/xarray.h>

int rust_helper_xa_err(void *entry)
{
	return xa_err(entry);
}

int rust_helper_xa_trylock(struct xarray *xa)
{
	return xa_trylock(xa);
}

void rust_helper_xa_lock(struct xarray *xa)
{
	return xa_lock(xa);
}

void rust_helper_xa_unlock(struct xarray *xa)
{
	return xa_unlock(xa);
}
