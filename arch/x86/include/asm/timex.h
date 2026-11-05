/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_TIMEX_H
#define _ASM_X86_TIMEX_H

#include <asm/processor.h>
#include <asm/tsc.h>

static inline unsigned long random_get_entropy(void)
{
	return rdtsc();
}
#define random_get_entropy random_get_entropy

#endif /* _ASM_X86_TIMEX_H */
