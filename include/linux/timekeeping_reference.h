/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TIMEKEEPING_REFERENCE_H
#define _LINUX_TIMEKEEPING_REFERENCE_H

#include <linux/clocksource_ids.h>
#include <linux/types.h>

struct timekeeper;

/**
 * struct tk_reference - Absolute time reference for feed-forward timekeeping
 * @cs_id:		Clocksource counter this reference applies to
 * @counter_value:	Counter reading at the reference point
 * @cycle_interval:	Counter cycles per tick (for ntp_tick computation)
 * @time_sec:		Seconds (UTC) at the reference point
 * @time_frac_sec:	Fractional seconds (units of 1/2^64 second)
 * @period_frac_sec:	Counter period (units of 1/2^(64+shift) seconds)
 * @period_shift:	Additional shift for period fixed-point
 */
struct tk_reference {
	enum clocksource_ids	cs_id;
	u64			counter_value;
	u64			cycle_interval;
	u64			time_sec;
	u64			time_frac_sec;
	u64			period_frac_sec;
	u8			period_shift;
};

int timekeeping_set_reference(const struct tk_reference *ref);
bool timekeeping_has_reference(void);
void timekeeping_clear_reference(void);
bool timekeeping_ref_ahead(struct timekeeper *tk);

#endif /* _LINUX_TIMEKEEPING_REFERENCE_H */
