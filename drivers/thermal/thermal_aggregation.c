// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>

#include "thermal_core.h"

/**
 * thermal_zone_aggregate_validate() - check an aggregation before initialization
 * @aggregate: aggregation description to validate before coefficient initialization
 * @ops: operations of the destination thermal zone
 *
 * The caller must hold valid references to the source zones and keep the entries
 * stable. Take each source lock separately to inspect its parameters and state;
 * the caller must not hold a thermal zone lock.
 *
 * Aggregations of aggregated zones are not supported yet: every source must be
 * a registered, non-aggregated thermal zone. This also rejects
 * a source whose parameters already reference this aggregation description.
 * Source membership and non-aggregation must remain unchanged after validation.
 * Validation does not acquire references or prevent subsequent source removal.
 *
 * Before initialization, zero denotes an unspecified coefficient. Either all
 * coefficients must be supplied or all must be zero to request averaging
 * defaults. This presence check must not be applied after initialization:
 * zero is valid in the temperature calculation.
 *
 * Return: 0 on success, -EINVAL for an invalid description, partial coefficient
 * specification, aggregated sources or destination set_trips,
 * or -ENODEV for missing or exiting sources.
 */
static int __maybe_unused
thermal_zone_aggregate_validate(struct thermal_zone_aggregate *aggregate,
				const struct thermal_zone_device_ops *ops)
{
	unsigned int initialized = 0;
	unsigned int i;

	/* An aggregation needs a description and at least one source entry. */
	if (!aggregate || !aggregate->entries || !aggregate->num_entries)
		return -EINVAL;

	/*
	 * Destination operations must exist, but cannot program hardware windows:
	 * aggregate temperature boundaries do not directly map to source thresholds.
	 */
	if (!ops || ops->set_trips)
		return -EINVAL;

	for (i = 0; i < aggregate->num_entries; i++) {
		const struct thermal_zone_aggregate_entry *entry = &aggregate->entries[i];
		struct thermal_zone_device *source = entry->tz;

		/* Reject unresolved sources before inspecting their zone state. */
		if (IS_ERR_OR_NULL(source))
			return -ENODEV;
		/* Before initialization, zero requests a default contribution. */
		initialized += entry->coefficient != 0;

		guard(thermal_zone)(source);
		/* A reference protects lifetime, but does not prevent source exit. */
		if (source->state & TZ_STATE_FLAG_EXIT)
			return -ENODEV;
		/* Nested aggregation, including a direct self-dependency, is unsupported. */
		if (source->tzp && source->tzp->aggregate)
			return -EINVAL;
	}

	/*
	 * Require either a complete explicit combination or all-zero coefficients
	 * for averaging defaults; there is no defined default for a partial set.
	 */
	if (initialized && initialized != aggregate->num_entries)
		return -EINVAL;

	return 0;
}
