// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/units.h>

#include "thermal_core.h"

/**
 * thermal_zone_aggregate_init_coefficients() - initialize aggregation multipliers
 * @aggregate: aggregation description to initialize before publication
 *
 * The description must be validated before calling this helper. Before
 * initialization, zero denotes an unspecified coefficient. If all are zero,
 * generate approximately equal contributions summing to
 * PERMILLE, distributing the remainder in source order.
 * If all coefficients are nonzero, leave them unchanged without normalization.
 * Partial specification is rejected by thermal_zone_aggregate_validate().
 *
 * The zone parameters' offset and source pointers are not modified.
 *
 * Return: 0 on success or -EINVAL for invalid arguments. Entries are unchanged
 * on failure.
 */
static int __maybe_unused
thermal_zone_aggregate_init_coefficients(struct thermal_zone_aggregate *aggregate)
{
	unsigned int initialized = 0;
	unsigned int base, remainder, i;

	if (!aggregate || !aggregate->entries || !aggregate->num_entries)
		return -EINVAL;

	for (i = 0; i < aggregate->num_entries; i++)
		initialized += aggregate->entries[i].coefficient != 0;

	/*
	 * Validation has rejected partial specification. If coefficients are
	 * supplied, preserve the explicit affine combination without normalization.
	 */
	if (initialized)
		return 0;

	/*
	 * Current systems are far from aggregating 1000 sources, but enforce
	 * this limit for unlikely yet possible configurations. More sources
	 * than the scale would produce zero default coefficients, which denote
	 * uninitialized entries.
	 */
	if (aggregate->num_entries > PERMILLE)
		return -EINVAL;

	/*
	 * With no coefficients supplied, approximate the arithmetic mean of
	 * the source temperatures:
	 *
	 *     T = sum(coefficient[i] * temperature[i]) / 1000 + offset
	 *
	 * Each source should contribute equally, so its ideal coefficient is
	 * PERMILLE / num_entries. Integer division rounds this
	 * down, leaving a remainder that must be distributed to keep the total
	 * coefficient sum exactly equal to PERMILLE.
	 *
	 * Assign one additional unit to each of the first 'remainder' sources.
	 * This makes the distribution deterministic in source order, with any
	 * two coefficients differing by at most one. For example:
	 *
	 *     2 sources: 500, 500
	 *     3 sources: 334, 333, 333
	 *     6 sources: 167, 167, 167, 167, 166, 166
	 *
	 * Equal source temperatures therefore retain unity gain. For unequal
	 * temperatures, the result approximates their arithmetic mean with a
	 * small bias from coefficient rounding. The signed offset in the zone
	 * parameters remains independent and is applied after division.
	 *
	 * These defaults apply only when every coefficient is uninitialized;
	 * explicitly supplied coefficients are never normalized.
	 */
	base = PERMILLE / aggregate->num_entries;
	remainder = PERMILLE % aggregate->num_entries;
	for (i = 0; i < aggregate->num_entries; i++)
		aggregate->entries[i].coefficient = base + (i < remainder);

	return 0;
}

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

/**
 * thermal_zone_get_temp_aggregate() - read an aggregated zone temperature
 * @tz: aggregate zone with a previously validated aggregation description
 * @temp: output temperature in millicelsius, unchanged on failure
 *
 * The caller must hold @tz->lock and keep the aggregation description and each
 * source zone alive and stable throughout the call. Source references must be
 * acquired under appropriate lifetime protection before publishing the entries;
 * the aggregate lock alone does not protect source lifetime or detachment.
 * The description must have passed thermal_zone_aggregate_validate() before
 * publication. Aggregations of aggregated zones are not supported yet, and
 * sources must remain non-aggregated. The destination cannot be a source.
 *
 * Source readings are acquired sequentially, not as a simultaneous snapshot.
 * Each source callback runs under its zone lock, including the usual emulation
 * handling. Source callbacks must not re-enter a held thermal zone lock.
 *
 * Return: 0 on success, a source read error, -EAGAIN for a busy source, -ENODEV
 * for an exiting source, -ENODATA for an invalid source temperature, or -ERANGE
 * for arithmetic overflow or a result outside the int range.
 */
int thermal_zone_get_temp_aggregate(struct thermal_zone_device *tz, int *temp)
{
	struct thermal_zone_aggregate *aggregate = tz->tzp->aggregate;
	s64 sum = 0, product;
	unsigned int i;
	int source_temp, ret;

	lockdep_assert_held(&tz->lock);

	for (i = 0; i < aggregate->num_entries; i++) {
		const struct thermal_zone_aggregate_entry *entry = &aggregate->entries[i];
		struct thermal_zone_device *source = entry->tz;

		/*
		 * The aggregate lock is already held. Do not wait for another
		 * zone lock: return -EAGAIN so the normal update path can retry.
		 */
		if (!mutex_trylock(&source->lock))
			return -EAGAIN;

		if (source->state & TZ_STATE_FLAG_EXIT)
			ret = -ENODEV;
		else
			ret = __thermal_zone_get_temp(source, &source_temp);

		mutex_unlock(&source->lock);
		if (ret)
			return ret;
		if (source_temp <= THERMAL_TEMP_INVALID)
			return -ENODATA;

		/* Cast before multiplying and retain fractional terms until summation. */
		if (check_mul_overflow((s64)entry->coefficient, (s64)source_temp, &product) ||
		    check_add_overflow(sum, product, &sum))
			return -ERANGE;
	}

	/* Divide once, truncating toward zero, then apply the unscaled offset. */
	sum = div_s64(sum, PERMILLE);
	if (check_add_overflow(sum, (s64)tz->tzp->offset, &sum) ||
	    sum < INT_MIN || sum > INT_MAX)
		return -ERANGE;

	*temp = sum;
	return 0;
}
