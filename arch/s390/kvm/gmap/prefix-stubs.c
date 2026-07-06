// SPDX-License-Identifier: GPL-2.0
/*
 * These stubs are used when building gmap for non-s390 guests
 * that don't need prefix page tracking.
 */

#include <linux/kvm_host.h>
#include <linux/kvm_types.h>

#include "dat.h"
#include "prefix.h"

bool _gmap_unmap_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end, bool hint)
{
	return true;
}

int dat_set_prefix_notif_bit(union asce asce, gfn_t gfn)
{
	return 0;
}
