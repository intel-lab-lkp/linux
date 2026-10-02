/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_CLOCK_INLINED_H
#define _ASM_ARM64_CLOCK_INLINED_H

#include <linux/jump_label.h>
#include <asm/arch_timer.h>

struct clocksource;

/*
 * Set by the arch timer driver when it selects the physical counter
 * (kernel booted at EL2); otherwise the virtual counter is used. The
 * driver only advertises CLOCK_SOURCE_CAN_INLINE_READ when it selected
 * a plain (no erratum workaround) sysreg read matching this function.
 */
DECLARE_STATIC_KEY_FALSE(arch_timer_inline_read_pct);

static __always_inline u64 arch_inlined_clocksource_read(struct clocksource *cs)
{
	if (static_branch_likely(&arch_timer_inline_read_pct))
		return __arch_counter_get_cntpct();

	return __arch_counter_get_cntvct();
}

#endif
