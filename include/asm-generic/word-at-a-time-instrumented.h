/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_WORD_AT_A_TIME_INSTRUMENTED_H
#define _ASM_GENERIC_WORD_AT_A_TIME_INSTRUMENTED_H

#include <linux/instrumented.h>

/*
 * Each arch's asm/word-at-a-time.h defines arch_load_unaligned_zeropad()
 * as inline asm plus an exception table entry, so the compiler never sees
 * the actual memory access and cannot emit KASAN/KCSAN instrumentation
 * for it. Provide the common, instrumented load_unaligned_zeropad() that
 * every caller uses by wrapping the arch version with instrument_read().
 *
 * This is marked __always_inline, and arch_load_unaligned_zeropad() is
 * itself a tiny inline-asm-only function, so this compiles down to the
 * same inline asm as before plus instrument_read() (which itself compiles
 * to nothing when KASAN and KCSAN are both disabled) -- no actual call.
 */
static __always_inline unsigned long load_unaligned_zeropad(const void *addr)
{
	instrument_read(addr, 1);
	return arch_load_unaligned_zeropad(addr);
}

#endif /* _ASM_GENERIC_WORD_AT_A_TIME_INSTRUMENTED_H */
