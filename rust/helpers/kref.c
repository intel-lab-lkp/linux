// SPDX-License-Identifier: GPL-2.0

#include <linux/kref.h>

void rust_helper_kref_get(struct kref *kref)
{
	kref_get(kref);
}

void rust_helper_kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
	kref_put(kref, release);
}
