/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_BOOT_TIME_PRIMITIVES_H
#define __ASM_BOOT_TIME_PRIMITIVES_H

#include <asm/arch_timer.h>
#include <linux/math64.h>

static inline u64 arch_boot_counter_now(void)
{
	return ((arch_timer_read_cntvct_el0() * 1000000) / arch_timer_get_cntfrq());
}

#endif
