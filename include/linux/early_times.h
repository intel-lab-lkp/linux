/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _KERNEL_PRINTK_EARLY_TIMES_H
#define _KERNEL_PRINTK_EARLY_TIMES_H

#include <linux/timex.h>

#if defined(CONFIG_EARLY_PRINTK_TIMES)
extern u32 early_mult, early_shift;
extern u64 early_ts_offset;

static inline u64 early_cycles(void)
{
	return ((u64)get_cycles() | (1ULL << 63));
}

static inline u64 adjust_early_ts(u64 ts)
{
	/* High bit means ts is a cycle count */
	if (unlikely(ts & (1ULL << 63)))
		/*
		 * mask high bit and convert to ns
		 * Note that early_mult may be 0, but that's OK because
		 * we'll just multiply by 0 and return 0. This will
		 * only occur if we're outputting a printk message
		 * before the calibration of the early timestamp.
		 * Any output after user space start (eg. from dmesg or
		 * journalctl) will show correct values.
		 */
		return (((ts & ~(1ULL << 63)) * early_mult) >> early_shift);

	/* If timestamp is already in ns, just add offset */
	return ts + early_ts_offset;
}
#else
static inline u64 early_cycles(void)
{
	return 0;
}

static inline u64 adjust_early_ts(u64 ts)
{
	return ts;
}
#endif /* CONFIG_EARLY_PRINTK_TIMES */

#endif /* _KERNEL_PRINTK_EARLY_TIMES_H */

