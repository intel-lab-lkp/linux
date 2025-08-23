/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_BOOT_TRACKER_H
#define _LINUX_BOOT_TRACKER_H

#include <linux/types.h>

enum kernel_bootstage_id {
	BOOTSTAGE_ID_KERNEL_START = 300,
	BOOTSTAGE_ID_KERNEL_END = 301,
};

/* Return boot time in nanoseconds using hardware counter */
u64 boot_time_now(void);

#endif
