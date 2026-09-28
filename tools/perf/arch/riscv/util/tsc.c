// SPDX-License-Identifier: GPL-2.0

#include <linux/types.h>

#include "../../../util/tsc.h"

u64 rdtsc(void)
{
	u64 val;

	/*
	 * The time CSR ticks at a constant frequency and is the same counter
	 * the kernel feeds into the user page conversion fields through
	 * sched_clock(), so it is a TSC equivalent here.
	 */
	asm volatile("rdtime %0" : "=r" (val));

	return val;
}
