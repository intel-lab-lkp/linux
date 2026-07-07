// SPDX-License-Identifier: GPL-2.0-only
/*
 * Track recoverable hardware errors (visible to the OS but not fatal) so that
 * crash tools like crash/drgn can read the count and timestamp of the last
 * occurrence from a vmcore and correlate them with a subsequent panic.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates
 * Copyright (c) 2026 Breno Leitao <leitao@kernel.org>
 */

#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/ras.h>
#include <linux/timekeeping.h>

struct hwerr_info {
	atomic_t count;
	time64_t timestamp;
};

/*
 * Keep hwerr_data[] at global scope so it stays accessible from the vmcore
 * (via crash/drgn) even when Link Time Optimization (LTO) is enabled.
 */
struct hwerr_info hwerr_data[HWERR_RECOV_MAX];

void hwerr_log_error_type(enum hwerr_error_type src)
{
	if (src < 0 || src >= HWERR_RECOV_MAX)
		return;

	atomic_inc(&hwerr_data[src].count);
	WRITE_ONCE(hwerr_data[src].timestamp, ktime_get_real_seconds());
}
EXPORT_SYMBOL_GPL(hwerr_log_error_type);
