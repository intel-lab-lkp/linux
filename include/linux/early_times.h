/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _KERNEL_PRINTK_EARLY_TIMES_H
#define _KERNEL_PRINTK_EARLY_TIMES_H

#include <linux/timex.h>
#include <linux/clocksource.h>

/* use high bit of a u64 to indicate cycles instead of a timestamp */
#define EARLY_CYCLES_BIT	BIT_ULL(63)
#define EARLY_CYCLES_MASK	~(BIT_ULL(63))

#if defined(CONFIG_EARLY_PRINTK_TIMES)
extern cycles_t start_cycles;
extern u64 start_ns;
extern u32 early_mult, early_shift;
extern u64 early_ts_offset;

static inline void early_times_start_calibration(void)
{
	start_cycles = get_cycles();
	start_ns = local_clock();
}

static inline void early_times_finish_calibration(void)
{
	cycles_t end_cycles;
	u64 end_ns;

	/* set calibration data for early_printk_times */
	end_cycles = get_cycles();
	end_ns = local_clock();
	clocks_calc_mult_shift(&early_mult, &early_shift,
		mul_u64_u64_div_u64(end_cycles - start_cycles,
			NSEC_PER_SEC, end_ns - start_ns),
		NSEC_PER_SEC, 100);
	early_ts_offset = mul_u64_u32_shr(start_cycles, early_mult, early_shift) - start_ns;

	pr_debug("Early printk times: mult=%u, shift=%u, offset=%llu ns\n",
		early_mult, early_shift, early_ts_offset);
}

static inline u64 early_cycles(void)
{
	return (get_cycles() | EARLY_CYCLES_BIT);
}

/*
 * adjust_early_ts detects whether ts in is cycles or nanoseconds
 * and converts it or adjusts it, taking into account the offset
 * from cycle-counter start.
 *
 * Note that early_mult may be 0, but that's OK because
 * we'll just multiply by 0 and return 0. This will
 * only occur if we're outputting a printk message
 * before the calibration of the early timestamp.
 * Any output after user space start (eg. from dmesg or
 * journalctl) will show correct values.
 */
static inline u64 adjust_early_ts(u64 ts)
{
	if (likely(!(ts & EARLY_CYCLES_BIT)))
		/* if timestamp is not in cycles, just add offset */
		return ts + early_ts_offset;

	/* mask high bit and convert to nanoseconds */
	return mul_u64_u32_shr(ts & EARLY_CYCLES_MASK, early_mult, early_shift);
}

#else
# define early_times_start_calibration() do { } while (0)
# define early_times_finish_calibration() do { } while (0)

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
