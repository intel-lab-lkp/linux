/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TIMEKEEPING_REFERENCE_H
#define _LINUX_TIMEKEEPING_REFERENCE_H

#include <linux/clocksource_ids.h>
#include <linux/types.h>

struct tk_reference {
	enum clocksource_ids	cs_id;
	u64			counter_value;
	u64			time_sec;
	u64			time_frac_sec;
	u64			period_frac_sec;
	u8			period_shift;
};

int timekeeping_set_reference(const struct tk_reference *ref);

#endif
