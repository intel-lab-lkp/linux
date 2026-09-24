// SPDX-License-Identifier: GPL-2.0
/*
 * Opt-in tracking of sleeping lock holders. See linux/lockholder.h.
 */
#include <linux/export.h>
#include <linux/lockholder.h>

/* Exported for inline lock functions used by modules. */
DEFINE_STATIC_KEY_FALSE(lock_holder_tracking_key);
EXPORT_SYMBOL_GPL(lock_holder_tracking_key);

void lock_holder_tracking_enable(void)
{
	/* static_branch_enable() takes cpus_read_lock() even if already on. */
	if (static_key_enabled(&lock_holder_tracking_key))
		return;

	static_branch_enable(&lock_holder_tracking_key);
}
