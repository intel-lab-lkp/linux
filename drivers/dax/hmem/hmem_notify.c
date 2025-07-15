// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/spinlock.h>
#include <linux/dax.h>

static walk_hmem_fn hmem_fallback_fn;
static DEFINE_SPINLOCK(hmem_notify_lock);

void hmem_register_fallback_handler(walk_hmem_fn hmem_fn)
{
	guard(spinlock_irqsave)(&hmem_notify_lock);
	hmem_fallback_fn = hmem_fn;
}
EXPORT_SYMBOL_GPL(hmem_register_fallback_handler);

void hmem_fallback_register_device(int target_nid, const struct resource *res)
{
	walk_hmem_fn hmem_fn;

	guard(spinlock)(&hmem_notify_lock);
	hmem_fn = hmem_fallback_fn;

	if (hmem_fn)
		hmem_fn(target_nid, res);
}
EXPORT_SYMBOL_GPL(hmem_fallback_register_device);
