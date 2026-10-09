/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_CLOCK_INLINED_H
#define _ASM_RISCV_CLOCK_INLINED_H

#include <asm/timex.h>

struct clocksource;

static __always_inline u64 arch_inlined_clocksource_read(struct clocksource *cs)
{
	return get_cycles64();
}

#endif
