/* SPDX-License-Identifier: GPL-2.0 */

#ifndef ARCH_KVM_GMAP_PREFIX_H
#define ARCH_KVM_GMAP_PREFIX_H

#include <linux/types.h>
#include <linux/kvm_types.h>

struct gmap;
union asce;

/**
 * _gmap_unmap_prefix() - Notify vCPUs if prefix pages are affected
 * @gmap: The gmap
 * @gfn: Start of the range
 * @end: End of the range (exclusive)
 * @hint: If true, skip notification if vCPU is in SIE
 *
 * Check if any vCPU's prefix pages fall within the given range and
 * request prefix refresh if needed.
 *
 * Return: false if notification was skipped due to hint, true otherwise
 */
bool _gmap_unmap_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end, bool hint);

/**
 * dat_set_prefix_notif_bit() - Set prefix notification bits
 * @asce: The address space control element
 * @gfn: Guest frame number of the prefix area
 *
 * Set the prefix notification bit in the page table entries for the
 * two prefix pages starting at @gfn.
 *
 * Return: 0 on success, -EAGAIN if not all bits could be set
 */
int dat_set_prefix_notif_bit(union asce asce, gfn_t gfn);

/**
 * gmap_mkold_prefix() - Mark prefix pages as old
 * @gmap: The gmap
 * @gfn: Start of the range
 * @end: End of the range (exclusive)
 *
 * Inline wrapper that calls _gmap_unmap_prefix with hint=true.
 *
 * Return: Result from _gmap_unmap_prefix
 */
static inline bool gmap_mkold_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end)
{
	return _gmap_unmap_prefix(gmap, gfn, end, true);
}

/**
 * gmap_unmap_prefix() - Unconditionally notify about prefix pages
 * @gmap: The gmap
 * @gfn: Start of the range
 * @end: End of the range (exclusive)
 *
 * Inline wrapper that calls _gmap_unmap_prefix with hint=false.
 *
 * Return: Result from _gmap_unmap_prefix
 */
static inline bool gmap_unmap_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end)
{
	return _gmap_unmap_prefix(gmap, gfn, end, false);
}

#endif /* ARCH_KVM_GMAP_PREFIX_H */
