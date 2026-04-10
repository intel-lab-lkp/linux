/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _EARLY_TIMES_H
#define _EARLY_TIMES_H

#include <linux/timekeeping.h>
#ifdef CONFIG_ARM64
#include <asm/sysreg.h>
#endif

#ifdef CONFIG_EARLY_CYCLES_KHZ
static inline u64 early_unsafe_cycles(void)
{
#if defined(CONFIG_X86_64)
	/*
	 * This rdtsc may happen before secure TSC is initialized, and
	 * it is unordered. So please don't use this value for cryptography
	 * or after SMP is initialized.
	 */
	return rdtsc();
#elif defined(CONFIG_ARM64)
	return read_sysreg(cntvct_el0);
#elif defined(CONFIG_RISCV_TIMER)
	u64 val;

	asm volatile("rdtime %0" : "=r"(val));
	return val;
#else
	return 0;
#endif
}

#define NS_PER_KHZ	1000000UL

/* returns a nanosecond value based on early cycles */
static inline u64 early_times_ns(void)
{
	if (CONFIG_EARLY_CYCLES_KHZ)
		/*
		 * Note: the multiply must precede the division to avoid
		 * truncation and loss of resolution
		 * Don't use fancier MULT/SHIFT math here.  Since this is
		 * static, the compiler can optimize the math operations.
		 */
		return (early_unsafe_cycles() * NS_PER_KHZ) / CONFIG_EARLY_CYCLES_KHZ;
	return 0;
}
#else
static inline u64 early_times_ns(void)
{
	return 0;
}
#endif

#endif /* _EARLY_TIMES_H */
