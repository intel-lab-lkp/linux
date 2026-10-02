// SPDX-License-Identifier: GPL-2.0
/*
 * thermal_bin.c - hardware-version bin-info registration for thermal zones
 *
 * Provides thermal_zone_set_supported_hw_bin() and friends, which allow platform
 * drivers to register SoC fuse / hardware-version information with a thermal
 * zone so that the OF trip-point parser can filter trips and select per-bin
 * temperatures via the 'thermal-hw-bin' and 'temperature-bin' DT properties.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#include "thermal_core.h"

/**
 * thermal_zone_set_supported_hw_bin - register hardware-version info with a zone
 * @tz:    thermal zone device
 * @vers:  array of hardware-version bitfields (one word per level)
 * @count: number of entries in @vers (must be <= THERMAL_HW_BIN_MAX_LEVELS)
 *
 * Stores a copy of @vers in @tz->hw_bin_info so that the thermal OF parser can
 * match trip-point thermal-hw-bin sub-groups against the running hardware.
 * The copy is managed by the zone's parent device via devm.
 *
 * Locking: the @supported_hw_bin pointer is the publish/consume gate for the
 * whole struct.  It is written last, with smp_store_release(), so that the
 * copied array contents and @supported_hw_bin_count are guaranteed visible to
 * any reader that observes a non-NULL pointer via thermal_zone_get_hw_bin_info()
 * (which uses the paired smp_load_acquire()).  This is a one-shot contract:
 * the platform driver publishes the bin info exactly once at probe, before the
 * zone is exposed to the OF trip parser, and the devm cleanup clears it once at
 * unbind.  There is no lock guarding concurrent publishers; callers must not
 * race two set/clear operations against each other.
 *
 * Return: 0 on success, negative errno on failure.
 */
int thermal_zone_set_supported_hw_bin(struct thermal_zone_device *tz,
				      const u32 *vers, unsigned int count)
{
	u32 *copy;

	if (WARN_ON(!tz || !vers || !count || count > THERMAL_HW_BIN_MAX_LEVELS))
		return -EINVAL;

	/*
	 * The copy is allocated against the zone's parent device so that it
	 * is freed when that device goes away.  Reject zones registered
	 * without a parent rather than scribbling on dev=NULL inside devm.
	 */
	if (!tz->device.parent)
		return -EINVAL;

	copy = devm_kmemdup(tz->device.parent, vers, count * sizeof(u32),
			    GFP_KERNEL);
	if (!copy)
		return -ENOMEM;

	tz->hw_bin_info.supported_hw_bin_count = count;
	/*
	 * Publish the pointer last: the release pairs with the acquire in
	 * thermal_zone_get_hw_bin_info() and orders the array contents and the
	 * count above before a reader can observe the non-NULL pointer.
	 */
	smp_store_release(&tz->hw_bin_info.supported_hw_bin, copy);

	return 0;
}
EXPORT_SYMBOL_GPL(thermal_zone_set_supported_hw_bin);

static void devm_thermal_zone_clear_supported_hw_bin(void *data)
{
	struct thermal_zone_device *tz = data;

	/*
	 * Retract the gate first with a release store so a reader either sees
	 * the fully-published info or nothing; the count is cleared after.
	 */
	smp_store_release(&tz->hw_bin_info.supported_hw_bin, NULL);
	tz->hw_bin_info.supported_hw_bin_count = 0;
}

/**
 * devm_thermal_zone_set_supported_hw_bin - managed variant of thermal_zone_set_supported_hw_bin
 * @dev:   device whose lifetime governs the cleanup action
 * @tz:    thermal zone device
 * @vers:  array of hardware-version bitfields
 * @count: number of entries in @vers
 *
 * Like thermal_zone_set_supported_hw_bin(), but registers a devm cleanup action on
 * @dev that clears the hw_bin_info when @dev is unbound.
 *
 * Return: 0 on success, negative errno on failure.
 */
int devm_thermal_zone_set_supported_hw_bin(struct device *dev,
					   struct thermal_zone_device *tz,
					   const u32 *vers, unsigned int count)
{
	int ret;

	ret = thermal_zone_set_supported_hw_bin(tz, vers, count);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev,
					devm_thermal_zone_clear_supported_hw_bin,
					tz);
}
EXPORT_SYMBOL_GPL(devm_thermal_zone_set_supported_hw_bin);

/**
 * thermal_zone_get_hw_bin_info - retrieve hardware-version info from a zone
 * @tz: thermal zone device (may be NULL)
 *
 * Return: pointer to the zone's thermal_hw_bin_info if supported_hw_bin has been
 *         set, or NULL if @tz is NULL or no hardware-version info is present.
 *
 * The gating read uses smp_load_acquire() to pair with the smp_store_release()
 * in thermal_zone_set_supported_hw_bin(): once a non-NULL pointer is observed,
 * the array contents and supported_hw_bin_count are guaranteed visible, so the
 * caller may dereference the returned struct's fields without further barriers.
 */
const struct thermal_hw_bin_info *
thermal_zone_get_hw_bin_info(const struct thermal_zone_device *tz)
{
	/* Pairs with smp_store_release() in thermal_zone_set_supported_hw_bin() */
	if (!tz || !smp_load_acquire(&tz->hw_bin_info.supported_hw_bin))
		return NULL;

	return &tz->hw_bin_info;
}
EXPORT_SYMBOL_GPL(thermal_zone_get_hw_bin_info);
